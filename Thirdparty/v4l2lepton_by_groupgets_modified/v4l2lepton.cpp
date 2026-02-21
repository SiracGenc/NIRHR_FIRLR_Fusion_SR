#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <linux/spi/spidev.h>
#include <getopt.h>

#include "Palettes.h"
#include "SPI.h"
#include "Lepton_I2C.h"

// ---- VoSPI ----
#define PACKET_SIZE 164
#define PACKET_SIZE_UINT16 (PACKET_SIZE/2)       // 82
#define PACKETS_PER_FRAME 60                     // payload packets per segment/frame (telemetry disabled case)
#define FRAME_SIZE_UINT16 (PACKET_SIZE_UINT16*PACKETS_PER_FRAME)

// ---- V4L2 ----
static const char *v4l2dev = "/dev/video1";
static const char *spidev_default = "/dev/spidev0.1"; // only for usage print
static char *spidev = NULL;                           // match SpiOpenPort(char*)

static int v4l2sink = -1;
static int width = 80;
static int height = 60;

enum OutFmt { OUT_RGB24 = 0, OUT_Y16 = 1 };
static int typeLepton = 2;     // 2 or 3
static enum OutFmt outFmt = OUT_RGB24;
static int typeColormap = 3;   // 1 rainbow, 2 grayscale, 3 ironblack
static int verbose = 0;

static int spi_mhz = 0;        // 0=keep SPI.cpp default (10MHz). If set, override after open.

static char *vidsendbuf = NULL;
static int vidsendsiz = 0;

static pthread_t sender;
static sem_t lock1, lock2;

// buffers: emulate raspberrypi_video logic
static uint8_t result[PACKET_SIZE * PACKETS_PER_FRAME];
static uint8_t shelf[4][PACKET_SIZE * PACKETS_PER_FRAME];

static inline int colormap_size(const int *cm) {
  int n = 0;
  while (cm[n] != -1) n++;
  return n;
}

static inline const int* pick_colormap(int cm) {
  switch (cm) {
    case 1: return colormap_rainbow;
    case 2: return colormap_grayscale;
    default: return colormap_ironblack;
  }
}

static void usage(const char *exec) {
  printf(
    "Usage: %s [options]\n"
    "Options:\n"
    "  -d | --device    <dev>     spidev device (default: %s)\n"
    "  -v | --video     <dev>     v4l2loopback device (default: %s)\n"
    "  -t | --type      2|3       Lepton type (2=80x60, 3=160x120)\n"
    "  -o | --out       rgb|y16   output format (default: rgb)\n"
    "  -c | --colormap  1|2|3     1=rainbow 2=grayscale 3=ironblack (default: 3)\n"
    "  -s | --spi-mhz   <N>       override SPI speed after open (e.g. 20)\n"
    "  -V | --verbose             debug prints\n"
    "  -h | --help\n",
    exec, spidev_default, v4l2dev
  );
}

static const char short_options[] = "d:hv:t:o:c:s:V";
static const struct option long_options[] = {
  { "device",    required_argument, NULL, 'd' },
  { "help",      no_argument,       NULL, 'h' },
  { "video",     required_argument, NULL, 'v' },
  { "type",      required_argument, NULL, 't' },
  { "out",       required_argument, NULL, 'o' },
  { "colormap",  required_argument, NULL, 'c' },
  { "spi-mhz",   required_argument, NULL, 's' },
  { "verbose",   no_argument,       NULL, 'V' },
  { 0, 0, 0, 0 }
};

static void open_vpipe() {
  if (typeLepton == 3) { width = 160; height = 120; }
  else { width = 80; height = 60; }

  v4l2sink = open(v4l2dev, O_WRONLY);
  if (v4l2sink < 0) {
    fprintf(stderr, "Failed to open v4l2sink device. (%s)\n", strerror(errno));
    exit(2);
  }

  struct v4l2_format v;
  memset(&v, 0, sizeof(v));
  v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

  if (ioctl(v4l2sink, VIDIOC_G_FMT, &v) < 0) {
    perror("VIDIOC_G_FMT");
    exit(3);
  }

  v.fmt.pix.width = width;
  v.fmt.pix.height = height;

  if (outFmt == OUT_Y16) {
    v.fmt.pix.pixelformat = V4L2_PIX_FMT_Y16;
    vidsendsiz = width * height * 2;
  } else {
    v.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
    vidsendsiz = width * height * 3;
  }
  v.fmt.pix.sizeimage = vidsendsiz;

  if (ioctl(v4l2sink, VIDIOC_S_FMT, &v) < 0) {
    perror("VIDIOC_S_FMT");
    exit(4);
  }

  vidsendbuf = (char*)malloc(vidsendsiz);
  if (!vidsendbuf) {
    fprintf(stderr, "malloc vidsendbuf failed\n");
    exit(5);
  }
  memset(vidsendbuf, 0, vidsendsiz);
}

// groupgets SPI.cpp exports spi_cs_fd and sets default speed=10MHz, mode3, 8bits. :contentReference[oaicite:5]{index=5}
extern int spi_cs_fd;

static void maybe_override_spi_speed() {
  if (spi_mhz <= 0) return;
  unsigned int hz = (unsigned int)spi_mhz * 1000U * 1000U;
  if (ioctl(spi_cs_fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) {
    perror("SPI_IOC_WR_MAX_SPEED_HZ");
  }
  if (verbose) {
    unsigned int readback = 0;
    if (ioctl(spi_cs_fd, SPI_IOC_RD_MAX_SPEED_HZ, &readback) == 0) {
      fprintf(stderr, "SPI speed set/readback: %u Hz\n", readback);
    }
  }
}

static void init_device() {
  SpiOpenPort(spidev); // if NULL -> /dev/spidev0.1 by SPI.cpp :contentReference[oaicite:6]{index=6}
  maybe_override_spi_speed();
}

static void stop_device() {
  SpiClosePort();
}

// discard packet (xFxx) is documented/used in examples :contentReference[oaicite:7]{index=7}
static inline bool is_discard_packet(const uint8_t *pkt) {
  return ((pkt[0] & 0x0F) == 0x0F);
}

static inline uint16_t be16_at(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

// Read one 60-packet block into `result`.
// For Lepton 3, segment number is extracted ONLY when packetNumber==20, as in LeptonThread.cpp logic.
static bool read_block(int *out_segmentNumber, int *out_resets) {
  int resets = 0;
  int segmentNumber = -1;

  for (int j = 0; j < PACKETS_PER_FRAME; j++) {
    uint8_t *pkt = result + PACKET_SIZE * j;

    if (read(spi_cs_fd, pkt, PACKET_SIZE) != PACKET_SIZE) {
      j = -1;
      resets++;
      usleep(1000);
      continue;
    }

    if (is_discard_packet(pkt)) {
      j = -1;
      resets++;
      usleep(1000);
      continue;
    }

    int packetNumber = pkt[1];
    if (packetNumber != j) {
      j = -1;
      resets++;
      usleep(1000);

      if (resets == 750) {
        // keep behavior consistent with example: close/open SPI
        SpiClosePort();
        usleep(750000);
        SpiOpenPort(spidev);
        maybe_override_spi_speed();
      }
      continue;
    }

    // Lepton 3: segmentNumber is encoded in packet 20 (TTT bits), see working example behavior.
    if ((typeLepton == 3) && (packetNumber == 20)) {
      segmentNumber = (pkt[0] >> 4) & 0x0F;
      if (segmentNumber < 1 || segmentNumber > 4) {
        if (verbose) fprintf(stderr, "[WARN] wrong segmentNumber=%d\n", segmentNumber);
        break;
      }
    }
  }

  if (resets >= 30 && verbose) {
    fprintf(stderr, "done reading, resets=%d\n", resets);
  }

  if (typeLepton == 3) {
    if (segmentNumber < 1 || segmentNumber > 4) {
      *out_segmentNumber = -1;
      *out_resets = resets;
      return false;
    }
    *out_segmentNumber = segmentNumber;
  } else {
    *out_segmentNumber = 1;
  }

  *out_resets = resets;
  return true;
}

static void render_rgb_from_shelf_lepton2() {
  const int *cm = pick_colormap(typeColormap);
  const int cmSize = colormap_size(cm);

  uint16_t minV = 65535, maxV = 0;

  // pass1: min/max
  for (int i = 0; i < FRAME_SIZE_UINT16; i++) {
    if (i % PACKET_SIZE_UINT16 < 2) continue;
    uint16_t v = (shelf[0][i*2] << 8) + shelf[0][i*2+1];
    if (v == 0) continue;
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }

  float diff = (float)maxV - (float)minV;
  float scale = (diff > 0.0f) ? (255.0f / diff) : 0.0f;
  memset(vidsendbuf, 0, vidsendsiz);

  // pass2: write pixels
  for (int i = 0; i < FRAME_SIZE_UINT16; i++) {
    if (i % PACKET_SIZE_UINT16 < 2) continue;

    uint16_t vfb = (shelf[0][i*2] << 8) + shelf[0][i*2+1];
    if (vfb == 0) continue;

    int column = (i % PACKET_SIZE_UINT16) - 2;
    int row = i / PACKET_SIZE_UINT16;
    if (column < 0 || column >= width || row < 0 || row >= height) continue;

    int value8 = (diff > 0.0f) ? (int)((vfb - minV) * scale) : 0;
    if (value8 < 0) value8 = 0;
    if (value8 > 255) value8 = 255;

    int ofs_r = 3 * value8 + 0; if (ofs_r >= cmSize) ofs_r = cmSize - 1;
    int ofs_g = 3 * value8 + 1; if (ofs_g >= cmSize) ofs_g = cmSize - 1;
    int ofs_b = 3 * value8 + 2; if (ofs_b >= cmSize) ofs_b = cmSize - 1;

    int idx = (row * width + column) * 3;
    vidsendbuf[idx + 0] = (char)cm[ofs_r];
    vidsendbuf[idx + 1] = (char)cm[ofs_g];
    vidsendbuf[idx + 2] = (char)cm[ofs_b];
  }

  if (verbose) fprintf(stderr, "L2 RGB min=%u max=%u\n", minV, maxV);
}

static void render_y16_from_shelf_lepton2() {
  uint16_t *out = (uint16_t*)vidsendbuf;
  memset(out, 0, vidsendsiz);

  for (int i = 0; i < FRAME_SIZE_UINT16; i++) {
    if (i % PACKET_SIZE_UINT16 < 2) continue;

    uint16_t vfb = (shelf[0][i*2] << 8) + shelf[0][i*2+1];

    int column = (i % PACKET_SIZE_UINT16) - 2;
    int row = i / PACKET_SIZE_UINT16;
    if (column < 0 || column >= width || row < 0 || row >= height) continue;

    out[row * width + column] = vfb;
  }
}

static bool render_frame_lepton3() {
  const int *cm = pick_colormap(typeColormap);
  const int cmSize = colormap_size(cm);

  uint16_t minV = 65535, maxV = 0;

  // pass1: min/max over 4 segments (skip headers, skip zeros like example)
  for (int seg = 0; seg < 4; seg++) {
    for (int i = 0; i < FRAME_SIZE_UINT16; i++) {
      if (i % PACKET_SIZE_UINT16 < 2) continue;
      uint16_t v = (shelf[seg][i*2] << 8) + shelf[seg][i*2+1];
      if (v == 0) continue;
      if (v < minV) minV = v;
      if (v > maxV) maxV = v;
    }
  }

  float diff = (float)maxV - (float)minV;
  float scale = (diff > 0.0f) ? (255.0f / diff) : 0.0f;

  if (outFmt == OUT_RGB24) memset(vidsendbuf, 0, vidsendsiz);
  else memset(vidsendbuf, 0, vidsendsiz);

  for (int seg = 0; seg < 4; seg++) {
    int ofsRow = 30 * seg; // 30 rows per segment in telemetry-disabled case
    for (int i = 0; i < FRAME_SIZE_UINT16; i++) {
      if (i % PACKET_SIZE_UINT16 < 2) continue;

      uint16_t vfb = (shelf[seg][i*2] << 8) + shelf[seg][i*2+1];
      if (vfb == 0) continue;

      // same mapping as working example
      int column = (i % PACKET_SIZE_UINT16) - 2
                 + (width / 2) * ((i % (PACKET_SIZE_UINT16 * 2)) / PACKET_SIZE_UINT16);
      int row = (i / PACKET_SIZE_UINT16) / 2 + ofsRow;

      if (column < 0 || column >= width || row < 0 || row >= height) continue;

      if (outFmt == OUT_Y16) {
        uint16_t *out = (uint16_t*)vidsendbuf;
        out[row * width + column] = vfb;
      } else {
        int value8 = (diff > 0.0f) ? (int)((vfb - minV) * scale) : 0;
        if (value8 < 0) value8 = 0;
        if (value8 > 255) value8 = 255;

        int ofs_r = 3 * value8 + 0; if (ofs_r >= cmSize) ofs_r = cmSize - 1;
        int ofs_g = 3 * value8 + 1; if (ofs_g >= cmSize) ofs_g = cmSize - 1;
        int ofs_b = 3 * value8 + 2; if (ofs_b >= cmSize) ofs_b = cmSize - 1;

        int idx = (row * width + column) * 3;
        vidsendbuf[idx + 0] = (char)cm[ofs_r];
        vidsendbuf[idx + 1] = (char)cm[ofs_g];
        vidsendbuf[idx + 2] = (char)cm[ofs_b];
      }
    }
  }

  if (verbose) fprintf(stderr, "L3 %s min=%u max=%u\n", (outFmt==OUT_RGB24)?"RGB":"Y16", minV, maxV);
  return true;
}

static void grab_frame() {
  if (typeLepton == 2) {
    int segno = 1, resets = 0;
    if (!read_block(&segno, &resets)) return;
    memcpy(shelf[0], result, sizeof(result));
    if (outFmt == OUT_Y16) render_y16_from_shelf_lepton2();
    else render_rgb_from_shelf_lepton2();
    return;
  }

  // Lepton 3: collect 4 segments; tolerate occasional bad segments
  static bool got[4] = {false, false, false, false};
  static int badSegCount = 0;

  for (;;) {
    int segno = -1, resets = 0;
    if (!read_block(&segno, &resets)) {
      badSegCount++;
      if (verbose && (badSegCount % 12 == 0)) {
        fprintf(stderr, "[WARN] wrong segment continuously %d times\n", badSegCount);
      }
      continue;
    }

    if (segno < 1 || segno > 4) continue;

    // start of frame
    if (segno == 1) {
      got[0]=got[1]=got[2]=got[3]=false;
    }

    memcpy(shelf[segno - 1], result, sizeof(result));
    got[segno - 1] = true;

    if (segno == 4) {
      if (got[0] && got[1] && got[2] && got[3]) {
        badSegCount = 0;
        render_frame_lepton3();
        return;
      }
      // didn't collect full set, keep going (do NOT wipe everything on seg4)
    }
  }
}

static void *sendvid(void *v) {
  (void)v;
  for (;;) {
    sem_wait(&lock1);
    if (vidsendsiz != write(v4l2sink, vidsendbuf, vidsendsiz)) exit(1);
    sem_post(&lock2);
  }
}

int main(int argc, char **argv) {
  for (;;) {
    int index = 0;
    int c = getopt_long(argc, argv, short_options, long_options, &index);
    if (c == -1) break;

    switch (c) {
      case 'd': spidev = optarg; break;
      case 'v': v4l2dev = optarg; break;
      case 't': {
        int v = atoi(optarg);
        typeLepton = (v == 3) ? 3 : 2;
      } break;
      case 'o':
        outFmt = (strcmp(optarg, "y16") == 0) ? OUT_Y16 : OUT_RGB24;
        break;
      case 'c': {
        int v = atoi(optarg);
        if (v==1 || v==2 || v==3) typeColormap = v;
      } break;
      case 's':
        spi_mhz = atoi(optarg);
        if (spi_mhz < 1) spi_mhz = 0;
        break;
      case 'V':
        verbose = 1;
        break;
      case 'h':
      default:
        usage(argv[0]);
        return 0;
    }
  }

  open_vpipe();

  // Producer/consumer semaphores:
  // lock2 starts "available", lock1 starts "no frame ready"
  if (sem_init(&lock2, 0, 1) == -1) exit(1);
  if (sem_init(&lock1, 0, 0) == -1) exit(1);
  pthread_create(&sender, NULL, sendvid, NULL);

  struct timespec ts;

  for (;;) {
    fprintf(stderr, "Waiting for sink\n");

    // allow first frame attempt
    sem_wait(&lock2);

    init_device();

    for (;;) {
      grab_frame();
      sem_post(&lock1);

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 2;

      // if write blocks (no readers) we break and re-open SPI later
      if (sem_timedwait(&lock2, &ts)) break;
    }

    stop_device();
  }

  close(v4l2sink);
  return 0;
}
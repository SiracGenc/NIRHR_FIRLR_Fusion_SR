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

#define PACKET_SIZE 164
#define PACKET_SIZE_UINT16 (PACKET_SIZE/2)       // 82
#define PACKETS_PER_FRAME 60                     // payload packets per segment/frame (telemetry-disabled base)
#define FRAME_SIZE_UINT16 (PACKET_SIZE_UINT16*PACKETS_PER_FRAME)

// Colormap is 256 levels * 3 channels = 768 ints (do NOT scan until -1 to avoid OOB crash)
#define COLORMAP_SIZE 768

static const char *v4l2dev = "/dev/video1";
static const char *spidev_default = "/dev/spidev0.1";
static char *spidev = NULL;

static int v4l2sink = -1;
static int width = 80;
static int height = 60;

enum OutFmt { OUT_RGB24 = 0, OUT_Y16 = 1 };
static int typeLepton = 2;     // 2 or 3
static enum OutFmt outFmt = OUT_RGB24;
static int typeColormap = 3;   // 1 rainbow, 2 grayscale, 3 ironblack
static int verbose = 0;

static int spi_mhz = 0;

static char *vidsendbuf = NULL;
static int vidsendsiz = 0;

static pthread_t sender;
static sem_t lock1, lock2;

static uint8_t result[PACKET_SIZE * PACKETS_PER_FRAME];
static uint8_t shelf[4][PACKET_SIZE * PACKETS_PER_FRAME];

// Telemetry alignment: stash next segment's packet0 if we peek it
static bool stash_valid = false;
static uint8_t stash_pkt[PACKET_SIZE];

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
  SpiOpenPort(spidev);
  maybe_override_spi_speed();
}

static void stop_device() { SpiClosePort(); }

// discard packet pattern (xFxx)
static inline bool is_discard_packet(const uint8_t *pkt) {
  return ((pkt[0] & 0x0F) == 0x0F);
}

// Read 60 packets into `result`, keeping alignment.
// Key behaviors:
// - Lepton3 segment number is extracted at packetNumber==20 (same as reference LeptonThread.cpp logic).
// - After 60 packets, peek 1 packet to handle telemetry (61st packet) vs next segment packet0 (stash).
static bool read_block(int *out_segmentNumber, int *out_resets) {
  int resets = 0;
  int segmentNumber = -1;

  for (int j = 0; j < PACKETS_PER_FRAME; j++) {
    uint8_t *pkt = result + PACKET_SIZE * j;

    if (j == 0 && stash_valid) {
      memcpy(pkt, stash_pkt, PACKET_SIZE);
      stash_valid = false;
    } else {
      if (read(spi_cs_fd, pkt, PACKET_SIZE) != PACKET_SIZE) {
        j = -1;
        resets++;
        usleep(1000);
        continue;
      }
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
        SpiClosePort();
        usleep(750000);
        SpiOpenPort(spidev);
        maybe_override_spi_speed();
      }
      continue;
    }

    if ((typeLepton == 3) && (packetNumber == 20)) {
      int seg = (pkt[0] >> 4) & 0x0F;
      // seg can be 0 for invalid segments; accept it and let upper layer drop
      segmentNumber = seg;
    }
  }

  // Peek 1 packet to keep alignment with telemetry on/off.
  if (typeLepton == 3) {
    uint8_t peek[PACKET_SIZE];
    int r = read(spi_cs_fd, peek, PACKET_SIZE);
    if (r == PACKET_SIZE && !is_discard_packet(peek)) {
      int pn = peek[1];
      if (pn != 60) {
        memcpy(stash_pkt, peek, PACKET_SIZE);
        stash_valid = true;
      }
      // pn==60 => telemetry packet; discard it
    }
  }

  if (verbose && resets >= 30) {
    fprintf(stderr, "done reading, resets=%d\n", resets);
  }

  if (typeLepton == 3) {
    if (segmentNumber == -1) segmentNumber = 0;
    *out_segmentNumber = segmentNumber;
  } else {
    *out_segmentNumber = 1;
  }

  *out_resets = resets;
  return true;
}

static void render_frame_lepton3() {
  const int *cm = pick_colormap(typeColormap);
  const int cmSize = COLORMAP_SIZE;

  bool found = false;
  uint16_t minV = 65535, maxV = 0;

  for (int seg = 0; seg < 4; seg++) {
    for (int i = 0; i < FRAME_SIZE_UINT16; i++) {
      if (i % PACKET_SIZE_UINT16 < 2) continue;
      uint16_t v = (shelf[seg][i*2] << 8) + shelf[seg][i*2+1];
      if (v == 0) continue;
      found = true;
      if (v < minV) minV = v;
      if (v > maxV) maxV = v;
    }
  }

  if (!found) {
    memset(vidsendbuf, 0, vidsendsiz);
    if (verbose) fprintf(stderr, "L3: no valid pixels (all zeros). Output black frame.\n");
    return;
  }

  float diff = (float)maxV - (float)minV;
  float scale = (diff > 0.0f) ? (255.0f / diff) : 0.0f;

  memset(vidsendbuf, 0, vidsendsiz);

  for (int seg = 0; seg < 4; seg++) {
    int ofsRow = 30 * seg;
    for (int i = 0; i < FRAME_SIZE_UINT16; i++) {
      if (i % PACKET_SIZE_UINT16 < 2) continue;

      uint16_t vfb = (shelf[seg][i*2] << 8) + shelf[seg][i*2+1];
      if (vfb == 0) continue;

      int column = (i % PACKET_SIZE_UINT16) - 2
                 + (width / 2) * ((i % (PACKET_SIZE_UINT16 * 2)) / PACKET_SIZE_UINT16);
      int row = (i / PACKET_SIZE_UINT16) / 2 + ofsRow;

      if (column < 0 || column >= width || row < 0 || row >= height) continue;

      if (outFmt == OUT_Y16) {
        uint16_t *out = (uint16_t*)vidsendbuf;
        out[row * width + column] = vfb; // Y16 is little-endian in memory :contentReference[oaicite:4]{index=4}
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
}

static void render_lepton2() {
  const int *cm = pick_colormap(typeColormap);
  const int cmSize = COLORMAP_SIZE;

  bool found = false;
  uint16_t minV = 65535, maxV = 0;

  for (int i = 0; i < FRAME_SIZE_UINT16; i++) {
    if (i % PACKET_SIZE_UINT16 < 2) continue;
    uint16_t v = (shelf[0][i*2] << 8) + shelf[0][i*2+1];
    if (v == 0) continue;
    found = true;
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }

  if (!found) {
    memset(vidsendbuf, 0, vidsendsiz);
    return;
  }

  float diff = (float)maxV - (float)minV;
  float scale = (diff > 0.0f) ? (255.0f / diff) : 0.0f;

  memset(vidsendbuf, 0, vidsendsiz);

  for (int i = 0; i < FRAME_SIZE_UINT16; i++) {
    if (i % PACKET_SIZE_UINT16 < 2) continue;

    uint16_t vfb = (shelf[0][i*2] << 8) + shelf[0][i*2+1];

    int column = (i % PACKET_SIZE_UINT16) - 2;
    int row = i / PACKET_SIZE_UINT16;
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

static void grab_frame() {
  if (typeLepton == 2) {
    int segno = 1, resets = 0;
    (void)read_block(&segno, &resets);
    memcpy(shelf[0], result, sizeof(result));
    render_lepton2();
    return;
  }

  static bool got[4] = {false,false,false,false};
  static unsigned invalidSegs = 0;

  for (;;) {
    int segno = 0, resets = 0;
    (void)read_block(&segno, &resets);

    // segno==0 => invalid segment; drop quietly (it happens)
    if (segno < 1 || segno > 4) {
      invalidSegs++;
      if (verbose && (invalidSegs % 200 == 0)) {
        fprintf(stderr, "[INFO] invalid segments seen: %u (segno=%d)\n", invalidSegs, segno);
      }
      continue;
    }

    if (segno == 1) {
      got[0]=got[1]=got[2]=got[3]=false;
    }

    memcpy(shelf[segno - 1], result, sizeof(result));
    got[segno - 1] = true;

    if (segno == 4 && got[0] && got[1] && got[2] && got[3]) {
      invalidSegs = 0;
      render_frame_lepton3();
      return;
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
      case 't': typeLepton = (atoi(optarg) == 3) ? 3 : 2; break;
      case 'o': outFmt = (strcmp(optarg, "y16") == 0) ? OUT_Y16 : OUT_RGB24; break;
      case 'c': {
        int v = atoi(optarg);
        if (v==1 || v==2 || v==3) typeColormap = v;
      } break;
      case 's': spi_mhz = atoi(optarg); if (spi_mhz < 1) spi_mhz = 0; break;
      case 'V': verbose = 1; break;
      case 'h':
      default: usage(argv[0]); return 0;
    }
  }

  open_vpipe();

  if (sem_init(&lock2, 0, 1) == -1) exit(1);
  if (sem_init(&lock1, 0, 0) == -1) exit(1);
  pthread_create(&sender, NULL, sendvid, NULL);

  struct timespec ts;

  for (;;) {
    fprintf(stderr, "Waiting for sink\n");
    sem_wait(&lock2);

    init_device();

    for (;;) {
      grab_frame();
      sem_post(&lock1);

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 2;

      if (sem_timedwait(&lock2, &ts)) break;
    }

    stop_device();
  }

  close(v4l2sink);
  return 0;
}
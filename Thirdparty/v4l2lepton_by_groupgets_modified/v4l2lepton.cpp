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
#include <getopt.h>

#include "Palettes.h"
#include "SPI.h"
#include "Lepton_I2C.h"

#define PACKET_SIZE 164
#define PACKETS_PER_SEGMENT 60   // telemetry disabled (Lepton 2.x frame, Lepton 3.x segment)
#define FPS 27

static const char *v4l2dev = "/dev/video1";
static const char *spidev_default = "/dev/spidev0.1"; // 仅用于 usage 打印
static char *spidev = NULL;                            // 注意：改回 char*

static int v4l2sink = -1;
static int width = 80;
static int height = 60;

enum OutFmt { OUT_RGB24 = 0, OUT_Y16 = 1 };
static int typeLepton = 2;       // 2 or 3
static enum OutFmt outFmt = OUT_RGB24;
static int typeColormap = 3;     // 1 rainbow, 2 grayscale, 3 ironblack

static char *vidsendbuf = NULL;
static int vidsendsiz = 0;

static pthread_t sender;
static sem_t lock1, lock2;

// Lepton 2.x: 1 segment (full frame) = 60 packets
// Lepton 3.x: 4 segments/frame, each 60 packets (telemetry disabled)
static uint8_t shelf[4][PACKET_SIZE * PACKETS_PER_SEGMENT];

static inline const int* pick_colormap(int cm) {
  switch (cm) {
    case 1: return colormap_rainbow;
    case 2: return colormap_grayscale;
    default: return colormap_ironblack;
  }
}

static void usage(char *exec) {
  printf(
    "Usage: %s [options]\n"
    "Options:\n"
    " -d  | --device   name   spidev device (default %s)\n"
    " -v  | --video    name   v4l2loopback device (default %s)\n"
    " -tl | --type     2|3    Lepton type: 2=80x60, 3=160x120 (default 2)\n"
    " -of | --out      rgb|y16  output format (default rgb)\n"
    " -cm | --colormap 1|2|3  1=rainbow 2=grayscale 3=ironblack (default 3)\n"
    " -h  | --help\n",
    exec, spidev_default, v4l2dev
  );
}

static const char short_options[] = "d:hv:";
static const struct option long_options[] = {
  { "device",   required_argument, NULL, 'd' },
  { "help",     no_argument,       NULL, 'h' },
  { "video",    required_argument, NULL, 'v' },
  { "type",     required_argument, NULL, 1000 },
  { "out",      required_argument, NULL, 1001 },
  { "colormap", required_argument, NULL, 1002 },
  { 0, 0, 0, 0 }
};

static void open_vpipe() {
  // set resolution by Lepton type
  if (typeLepton == 3) { width = 160; height = 120; }
  else { width = 80; height = 60; }

  v4l2sink = open(v4l2dev, O_WRONLY);
  if (v4l2sink < 0) {
    fprintf(stderr, "Failed to open v4l2sink device. (%s)\n", strerror(errno));
    exit(-2);
  }

  struct v4l2_format v;
  memset(&v, 0, sizeof(v));
  v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

  if (ioctl(v4l2sink, VIDIOC_G_FMT, &v) < 0) {
    perror("VIDIOC_G_FMT");
    exit(-3);
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
    exit(-4);
  }

  vidsendbuf = (char*)malloc(vidsendsiz);
  if (!vidsendbuf) {
    fprintf(stderr, "malloc vidsendbuf failed\n");
    exit(-5);
  }
}

static void init_device() {
  SpiOpenPort(spidev);   // spidev==NULL 时，SPI.cpp 会默认打开 /dev/spidev0.1
}

static void stop_device() {
  SpiClosePort();
}

// discard packets: header ID high nibble == 0xF (xFxx) is a discard pattern in VoSPI streams
// packet header is 4 bytes, payload is 160 bytes in Raw14 mode. (packet total 164 bytes)
static inline bool is_discard_packet(const uint8_t *pkt) {
  return ((pkt[0] & 0x0F) == 0x0F);
}

static int read_one_segment(uint8_t *dst, int *out_segment_number) {
  int resets = 0;
  int segno = -1;

  for (int j = 0; j < PACKETS_PER_SEGMENT; j++) {
    if (read(spi_cs_fd, dst + (PACKET_SIZE * j), PACKET_SIZE) != PACKET_SIZE) {
      // treat as resync
      j = -1;
      resets++;
      usleep(1000);
      continue;
    }

    uint8_t *pkt = dst + (PACKET_SIZE * j);

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
      }
      continue;
    }

    if (j == 0) {
      segno = (pkt[0] >> 4) & 0x0F; // for Lepton 3.x this is 1..4
    } else if (typeLepton == 3) {
      int seg_j = (pkt[0] >> 4) & 0x0F;
      if (seg_j != segno) {
        j = -1;
        resets++;
        usleep(1000);
        continue;
      }
    }
  }

  *out_segment_number = segno;
  return resets;
}

static inline uint16_t read_be16(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

static void fill_output_from_values_rgb(const uint16_t *vals, int w, int h) {
  const int *cm = pick_colormap(typeColormap);

  uint16_t minV = 65535, maxV = 0;
  int n = w * h;
  for (int i = 0; i < n; i++) {
    uint16_t v = vals[i];
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }
  float diff = (float)maxV - (float)minV;
  float scale = (diff > 0.0f) ? (255.0f / diff) : 1.0f;

  for (int i = 0; i < n; i++) {
    uint16_t v = vals[i];
    int value8 = (int)((v - minV) * scale);
    if (value8 < 0) value8 = 0;
    if (value8 > 255) value8 = 255;

    int idx = i * 3;
    vidsendbuf[idx + 0] = (char)cm[3 * value8 + 0];
    vidsendbuf[idx + 1] = (char)cm[3 * value8 + 1];
    vidsendbuf[idx + 2] = (char)cm[3 * value8 + 2];
  }
}

static void fill_output_from_values_y16(const uint16_t *vals, int w, int h) {
  // V4L2_PIX_FMT_Y16: store little-endian 16-bit luma values
  uint16_t *out = (uint16_t*)vidsendbuf;
  int n = w * h;
  for (int i = 0; i < n; i++) out[i] = vals[i];
}

static void grab_frame() {
  static uint16_t values80x60[80 * 60];
  static uint16_t values160x120[160 * 120];

  if (typeLepton == 2) {
    int segno = -1;
    int resets = read_one_segment(shelf[0], &segno);
    if (resets >= 30) fprintf(stderr, "done reading, resets: %d\n", resets);

    // each packet is one full row, payload is 80 pixels (160 bytes), header 4 bytes
    for (int row = 0; row < 60; row++) {
      const uint8_t *pkt = shelf[0] + (PACKET_SIZE * row);
      const uint8_t *payload = pkt + 4;
      for (int x = 0; x < 80; x++) {
        values80x60[row * 80 + x] = read_be16(payload + x * 2);
      }
    }

    if (outFmt == OUT_Y16) fill_output_from_values_y16(values80x60, 80, 60);
    else fill_output_from_values_rgb(values80x60, 80, 60);

    return;
  }

  // Lepton 3.x: read segments 1..4
  bool got[4] = {false, false, false, false};

  for (;;) {
    int segno = -1;
    int resets = read_one_segment(shelf[0], &segno); // temporary read buffer (reuse shelf[0])
    if (resets >= 30) fprintf(stderr, "done reading, resets: %d\n", resets);

    if (segno < 1 || segno > 4) continue;

    memcpy(shelf[segno - 1], shelf[0], PACKET_SIZE * PACKETS_PER_SEGMENT);
    got[segno - 1] = true;

    if (segno == 4) {
      if (got[0] && got[1] && got[2] && got[3]) break;
      // missed earlier segments -> drop and resync next frame
      got[0] = got[1] = got[2] = got[3] = false;
    }
  }

  // assemble 160x120: each segment is 30 rows, each row uses 2 packets (80 px each)
  for (int seg = 0; seg < 4; seg++) {
    const uint8_t *segbuf = shelf[seg];
    int rowBase = seg * 30;

    for (int p = 0; p < 60; p++) {
      const uint8_t *pkt = segbuf + (PACKET_SIZE * p);
      const uint8_t *payload = pkt + 4;

      int row = rowBase + (p / 2);
      int colBase = (p % 2) * 80;

      for (int x = 0; x < 80; x++) {
        values160x120[row * 160 + colBase + x] = read_be16(payload + x * 2);
      }
    }
  }

  if (outFmt == OUT_Y16) fill_output_from_values_y16(values160x120, 160, 120);
  else fill_output_from_values_rgb(values160x120, 160, 120);
}

static void *sendvid(void *v) {
  (void)v;
  for (;;) {
    sem_wait(&lock1);
    if (vidsendsiz != write(v4l2sink, vidsendbuf, vidsendsiz)) exit(-1);
    sem_post(&lock2);
  }
}

int main(int argc, char **argv) {
  // parse args
  for (;;) {
    int index = 0;
    int c = getopt_long(argc, argv, short_options, long_options, &index);
    if (c == -1) break;

    switch (c) {
      case 'd': spidev = optarg; break;
      case 'v': v4l2dev = optarg; break;
      case 'h': usage(argv[0]); return 0;

      case 1000: { // --type
        int val = atoi(optarg);
        typeLepton = (val == 3) ? 3 : 2;
      } break;

      case 1001: { // --out
        if (strcmp(optarg, "y16") == 0) outFmt = OUT_Y16;
        else outFmt = OUT_RGB24;
      } break;

      case 1002: { // --colormap
        int val = atoi(optarg);
        if (val == 1 || val == 2 || val == 3) typeColormap = val;
      } break;

      default:
        usage(argv[0]);
        return 1;
    }
  }

  open_vpipe();

  if (sem_init(&lock2, 0, 1) == -1) exit(-1);
  sem_wait(&lock2);
  if (sem_init(&lock1, 0, 1) == -1) exit(-1);
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
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
#define VIDEO_PKTS_PER_SEG 60   // Lepton 3 telemetry disabled: 60 pkts/segment; enabled: 61 pkts/segment (handled by peek)
#define FPS 27

static const char *v4l2dev = "/dev/video1";
static const char *spidev_default = "/dev/spidev0.1"; // only for usage print
static char *spidev = NULL;                            // keep char* to match SpiOpenPort(char*)

static int v4l2sink = -1;
static int width = 80;
static int height = 60;

enum OutFmt { OUT_RGB24 = 0, OUT_Y16 = 1 };
static int typeLepton = 2;       // 2 or 3
static enum OutFmt outFmt = OUT_RGB24;
static int typeColormap = 3;     // 1 rainbow, 2 grayscale, 3 ironblack
static int verbose = 0;

static char *vidsendbuf = NULL;
static int vidsendsiz = 0;

static pthread_t sender;
static sem_t lock1, lock2;

// Lepton 2.x: one frame = 60 packets
// Lepton 3.x: one segment = 60 packets payload (plus optional telemetry packet)
static uint8_t shelf[4][PACKET_SIZE * VIDEO_PKTS_PER_SEG];
static uint8_t tmpseg[PACKET_SIZE * VIDEO_PKTS_PER_SEG];

// For Lepton 3 telemetry auto-handling: stash the first packet of the next segment if we peek it
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
    "  -d | --device   <dev>     spidev device (default: %s)\n"
    "  -v | --video    <dev>     v4l2loopback device (default: %s)\n"
    "  -t | --type     2|3       Lepton type (2=80x60, 3=160x120)\n"
    "  -o | --out      rgb|y16   output format (default: rgb)\n"
    "  -c | --colormap 1|2|3     1=rainbow 2=grayscale 3=ironblack (default: 3)\n"
    "  -V | --verbose            enable debug prints\n"
    "  -h | --help\n",
    exec, spidev_default, v4l2dev
  );
}

static const char short_options[] = "d:hv:t:o:c:V";
static const struct option long_options[] = {
  { "device",   required_argument, NULL, 'd' },
  { "help",     no_argument,       NULL, 'h' },
  { "video",    required_argument, NULL, 'v' },
  { "type",     required_argument, NULL, 't' },
  { "out",      required_argument, NULL, 'o' },
  { "colormap", required_argument, NULL, 'c' },
  { "verbose",  no_argument,       NULL, 'V' },
  { 0, 0, 0, 0 }
};

static void open_vpipe() {
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
  memset(vidsendbuf, 0, vidsendsiz);
}

static void init_device() {
  // SPI.cpp should handle default if spidev == NULL
  SpiOpenPort(spidev);
}

static void stop_device() {
  SpiClosePort();
}

// Lepton 3 datasheet: discard packet ID field is always xFxx (x = don't care)
// In bytes, this maps well to: (pkt[0] & 0x0F) == 0x0F  (low nibble of MSB byte) for MSB-first SPI streams.
static inline bool is_discard_packet(const uint8_t *pkt) {
  return ((pkt[0] & 0x0F) == 0x0F);
}

static inline int packet_number_8bit(const uint8_t *pkt) {
  // For our expected ranges (0..60), low byte is sufficient
  return (int)pkt[1];
}

static inline int lepton3_segment_from_packet20(const uint8_t *segbuf60pkts) {
  // Lepton 3 datasheet: segment number is encoded in TTT bits ONLY on packet number 20.
  // pkt[0] high nibble = 0TTT (first bit always 0), thus seg = pkt[0] >> 4, range 0..7
  const uint8_t *pkt20 = segbuf60pkts + (20 * PACKET_SIZE);
  int seg = (pkt20[0] >> 4) & 0x07;
  return seg; // 0 means invalid segment
}

static int read_exact_packet(uint8_t *dst) {
  int r = read(spi_cs_fd, dst, PACKET_SIZE);
  return r;
}

// Read one segment payload: always reads 60 packets of pixel payload into dst.
// For Lepton 3, after 60 packets it will peek one more packet to auto-handle telemetry (61st packet).
// Returns resets count; outputs segno for Lepton 3, or 0 for Lepton 2.
static int read_one_segment(uint8_t *dst60pkts, int *out_segment_number) {
  int resets = 0;

  for (int j = 0; j < VIDEO_PKTS_PER_SEG; j++) {
    uint8_t *pkt = dst60pkts + (PACKET_SIZE * j);

    // Use stashed packet as the first packet of a segment (Lepton 3 telemetry auto)
    if (j == 0 && stash_valid) {
      memcpy(pkt, stash_pkt, PACKET_SIZE);
      stash_valid = false;
    } else {
      if (read_exact_packet(pkt) != PACKET_SIZE) {
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

    int pn = packet_number_8bit(pkt);
    if (pn != j) {
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
  }

  // Determine segment number (Lepton 3 only) using packet 20 TTT bits
  int segno = 0;
  if (typeLepton == 3) {
    segno = lepton3_segment_from_packet20(dst60pkts);
  }
  *out_segment_number = segno;

  // Telemetry auto handling (Lepton 3 only):
  // telemetry enabled => 61 packets/segment, packetNumber 60 exists and must be consumed
  // telemetry disabled => next packet is packet 0 of next segment; stash it
  if (typeLepton == 3) {
    uint8_t peek[PACKET_SIZE];
    if (read_exact_packet(peek) == PACKET_SIZE) {
      if (!is_discard_packet(peek)) {
        int pn = packet_number_8bit(peek);
        if (pn != 60) {
          // Not the telemetry packet => it's the first packet of the next segment
          memcpy(stash_pkt, peek, PACKET_SIZE);
          stash_valid = true;
        } else {
          // pn==60 => telemetry packet, discard it to keep alignment
        }
      } else {
        // discard packet in idle gap; ignore
      }
    }
  }

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
  float scale = (diff > 0.0f) ? (255.0f / diff) : 0.0f;

  for (int i = 0; i < n; i++) {
    uint16_t v = vals[i];
    int value8 = 0;
    if (diff > 0.0f) {
      value8 = (int)((v - minV) * scale);
      if (value8 < 0) value8 = 0;
      if (value8 > 255) value8 = 255;
    } else {
      // flat frame; keep black
      value8 = 0;
    }

    int idx = i * 3;
    vidsendbuf[idx + 0] = (char)cm[3 * value8 + 0];
    vidsendbuf[idx + 1] = (char)cm[3 * value8 + 1];
    vidsendbuf[idx + 2] = (char)cm[3 * value8 + 2];
  }

  if (verbose) {
    fprintf(stderr, "RGB frame: min=%u max=%u diff=%u\n", minV, maxV, (unsigned)(maxV - minV));
  }
}

static void fill_output_from_values_y16(const uint16_t *vals, int w, int h) {
  // V4L2_PIX_FMT_Y16 expects little-endian; Raspberry Pi is little-endian.
  uint16_t *out = (uint16_t*)vidsendbuf;
  int n = w * h;
  for (int i = 0; i < n; i++) out[i] = vals[i];
}

static bool assemble_lepton2_frame() {
  static uint16_t values80x60[80 * 60];

  int segno = 0;
  int resets = read_one_segment(shelf[0], &segno);
  if (verbose && resets >= 1) fprintf(stderr, "Lepton2 read resets=%d\n", resets);

  for (int row = 0; row < 60; row++) {
    const uint8_t *pkt = shelf[0] + (PACKET_SIZE * row);
    const uint8_t *payload = pkt + 4;
    for (int x = 0; x < 80; x++) {
      values80x60[row * 80 + x] = read_be16(payload + x * 2);
    }
  }

  if (outFmt == OUT_Y16) fill_output_from_values_y16(values80x60, 80, 60);
  else fill_output_from_values_rgb(values80x60, 80, 60);

  return true;
}

static bool assemble_lepton3_frame() {
  static uint16_t values160x120[160 * 120];
  bool got[5] = {false, false, false, false, false}; // index 1..4 used

  // To avoid starvation, bound attempts; keep last frame if cannot assemble
  const int MAX_ATTEMPTS = 200;
  int attempts = 0;

  while (attempts++ < MAX_ATTEMPTS) {
    int segno = 0;
    int resets = read_one_segment(tmpseg, &segno);
    if (verbose && resets >= 1) fprintf(stderr, "Lepton3 segment read resets=%d segno=%d\n", resets, segno);

    // Lepton 3 datasheet: segno==0 means invalid segment; ignore
    if (segno < 1 || segno > 4) {
      continue;
    }

    // Treat segment 1 as start-of-frame to drop partial frames
    if (segno == 1) {
      got[1] = got[2] = got[3] = got[4] = false;
    }

    memcpy(shelf[segno - 1], tmpseg, PACKET_SIZE * VIDEO_PKTS_PER_SEG);
    got[segno] = true;

    if (got[1] && got[2] && got[3] && got[4]) {
      // Assemble 160x120: each segment contributes 30 rows; each row uses 2 packets (80 px each)
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

      return true;
    }
  }

  if (verbose) fprintf(stderr, "Lepton3: failed to assemble full frame within %d attempts\n", MAX_ATTEMPTS);
  return false;
}

static void grab_frame() {
  if (typeLepton == 2) {
    assemble_lepton2_frame();
  } else {
    // Only update buffer when a full frame is assembled; otherwise keep last frame
    (void)assemble_lepton3_frame();
  }
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
  for (;;) {
    int index = 0;
    int c = getopt_long(argc, argv, short_options, long_options, &index);
    if (c == -1) break;

    switch (c) {
      case 'd': spidev = optarg; break;
      case 'v': v4l2dev = optarg; break;
      case 't': {
        int val = atoi(optarg);
        typeLepton = (val == 3) ? 3 : 2;
      } break;
      case 'o': {
        if (strcmp(optarg, "y16") == 0) outFmt = OUT_Y16;
        else outFmt = OUT_RGB24;
      } break;
      case 'c': {
        int val = atoi(optarg);
        if (val == 1 || val == 2 || val == 3) typeColormap = val;
      } break;
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
  // lock2: buffer available (start as 1)
  // lock1: frame ready (start as 0)
  if (sem_init(&lock2, 0, 1) == -1) exit(-1);
  if (sem_init(&lock1, 0, 0) == -1) exit(-1);
  pthread_create(&sender, NULL, sendvid, NULL);

  struct timespec ts;

  for (;;) {
    fprintf(stderr, "Waiting for sink\n");
    // wait until buffer is available
    sem_wait(&lock2);

    init_device();

    for (;;) {
      grab_frame();
      sem_post(&lock1);

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 2;

      // wait for frame to be written; if writer blocks (no sink), timeout and re-open SPI later
      if (sem_timedwait(&lock2, &ts)) break;
    }

    stop_device();
  }

  close(v4l2sink);
  return 0;
}
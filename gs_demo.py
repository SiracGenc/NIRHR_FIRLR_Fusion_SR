import sys
import time
import cv2
import numpy as np
from picamera2 import Picamera2
from picamera2.encoders import H264Encoder
from picamera2.outputs import FfmpegOutput
from libcamera import Transform

WIDTH, HEIGHT = 1456, 1088
TARGET_FPS = 60
INITIAL_BITRATE = 20_000_000 
RECORDING_PREFIX = "gs_60fps"

class Industrial60FPSDemo:
    def __init__(self):
        self.picam2 = Picamera2()
        self.encoder = None
        self.output = None
        self.recording = False
        self.bitrate = INITIAL_BITRATE
        self.frame_count = 0
        self.last_time = time.time()
        self.fps = 0
#config
        video_config = self.picam2.create_video_configuration(
            main={"size": (WIDTH, HEIGHT), "format": "RGB888"},
            controls={
                "FrameRate": TARGET_FPS,
                "AeEnable": False,          
                "ExposureTime": 16000,      # 1/60s
                "AnalogueGain": 1.0,
                "AwbEnable": True,
            },
            transform=Transform(hflip=False, vflip=False)
        )
        self.picam2.configure(video_config)
        self.picam2.start()
        print(f"60 FPS ACTIVE: {WIDTH}x{HEIGHT} @ {TARGET_FPS} FPS")

    def update_encoder(self):
        if self.encoder:
            self.encoder.stop()
        self.encoder = H264Encoder(bitrate=self.bitrate)

    def toggle_recording(self):
        if self.recording:
            self.picam2.stop_encoder()
            print(f"Recording stopped: {self.output.filename}")
            self.recording = False
        else:
            filename = f"{RECORDING_PREFIX}_{time.strftime('%Y%m%d-%H%M%S')}.mp4"
            self.output = FfmpegOutput(filename)
            self.update_encoder()
            self.picam2.start_encoder(self.encoder, self.output)
            print(f"60 FPS Recording: {filename}")
            self.recording = True

    def adjust_bitrate(self, delta):
        self.bitrate = int(np.clip(self.bitrate + delta, 5_000_000, 30_000_000))
        print(f" Bitrate: {self.bitrate // 1_000_000} Mbps")
        if self.recording:
            self.toggle_recording()
            self.toggle_recording()

    def save_frame(self, frame):
        filename = f"gs_60fps_{int(time.time())}.png"
        cv2.imwrite(filename, frame)
        print(f"Picture Saved: {filename}")

    def get_system_stats(self):
        try:
            with open("/sys/class/thermal/thermal_zone0/temp", "r") as f:
                temp = int(f.read()) / 1000.0
        except:
            temp = 0.0
        return temp

    def draw_industrial_ui(self, frame, stats):
        panel_h = 120
        cv2.rectangle(frame, (0, 0), (WIDTH, panel_h), (10, 10, 10), -1)
        cv2.rectangle(frame, (0, HEIGHT - 60), (WIDTH, HEIGHT), (10, 10, 10), -1)
        
        font = cv2.FONT_HERSHEY_SIMPLEX
        font_scale = 0.7
        color = (0, 255, 0)
        y_offset = 35

        cv2.putText(frame, f"FPS: {stats['fps']:.1f}", (20, y_offset), font, font_scale, color, 2)
        cv2.putText(frame, f"60FPS MODE", (180, y_offset), font, font_scale, (0, 255, 255), 2)
        cv2.putText(frame, f"EXP: {stats['exp']:.1f}MS", (380, y_offset), font, font_scale, color, 2)
        cv2.putText(frame, f"BITRATE: {self.bitrate//1_000_000} MBPS", (580, y_offset), font, font_scale, color, 2)
        cv2.putText(frame, f"TEMP: {stats['temp']:.1f}C", (850, y_offset), font, font_scale, color, 2)

        mode_text = "RECORDING 60FPS" if self.recording else "LIVE 60FPS"
        mode_color = (0, 0, 255) if self.recording else (0, 255, 255)
        cv2.putText(frame, mode_text, (WIDTH - 250, y_offset), font, font_scale, mode_color, 2)

        footer = "[Q] QUIT  [R] REC  [S] SNAP  [+/–] BITRATE"
        cv2.putText(frame, footer, (20, HEIGHT - 20), font, 0.6, (200, 200, 200), 1)
        cv2.rectangle(frame, (0, 0), (WIDTH - 1, HEIGHT - 1), (50, 50, 50), 2)

    def run(self):
        print("\n" + "="*60)
        print("60 FPS INDUSTRIAL DEMO — CONTROLS")
        print("[Q] Quit  [R] Toggle Recording  [S] Save Frame")
        print("[+] Increase Bitrate  [-] Decrease Bitrate")
        print("AE is LOCKED for 60 FPS stability")
        print("="*60)

        while True:
            frame = self.picam2.capture_array("main")
            self.frame_count += 1

            if self.frame_count % 60 == 0:
                now = time.time()
                self.fps = 60 / (now - self.last_time)
                self.last_time = now

            metadata = self.picam2.capture_metadata()
            exp_ms = metadata.get('ExposureTime', 0) / 1000.0
            temp = self.get_system_stats()

            stats = {'fps': self.fps, 'exp': exp_ms, 'temp': temp}
            self.draw_industrial_ui(frame, stats)

            cv2.imshow("RPi Global Shutter - 60 FPS", frame)
            key = cv2.waitKey(1) & 0xFF

            if key in (ord('q'), 27):
                break
            elif key == ord('r'):
                self.toggle_recording()
            elif key == ord('s'):
                self.save_frame(frame)
            elif key == ord('+'):
                self.adjust_bitrate(2_000_000)  
            elif key == ord('-'):
                self.adjust_bitrate(-2_000_000) 

        if self.recording:
            self.toggle_recording()
        self.picam2.stop()
        cv2.destroyAllWindows()
        print("\n 60 FPS Shutdown complete")

if __name__ == "__main__":
    try:
        demo = Industrial60FPSDemo()
        demo.run()
    except KeyboardInterrupt:
        print("\n Interrupted by user")
    except Exception as e:
        print(f"\n!!!! Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
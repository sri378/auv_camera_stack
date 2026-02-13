# AUV Camera V4L2 Control Message
# Groov-e USB Camera (1920x1080 MJPEG @ 25fps on Jetson Orin Nano)

int32 brightness           # Range: -64 to 64  | Default: 0
int32 contrast             # Range:   0 to 64  | Default: 36
int32 saturation           # Range:   0 to 128 | Default: 65
int32 hue                  # Range: -40 to 40  | Default: 6
bool  white_balance_auto   # true=auto, false=manual | Default: true
int32 white_balance_temp   # Range: 2800-6500 K (only when manual) | Default: 4600
int32 gamma                # Range:  72 to 500 | Default: 100
int32 gain                 # Range:   0 to 100 | Default: 0
int32 sharpness            # Range:   0 to 6   | Default: 3
int32 backlight_comp       # Range:   0 to 160 | Default: 30
bool  auto_exposure        # true=aperture priority, false=manual | Default: true
int32 exposure_time        # Manual exposure value (when auto_exposure=false)
int32 power_line_freq      # 0=Disabled, 1=50Hz, 2=60Hz | Default: 1

# AUV Camera Pipeline Status
std_msgs/Header header
string device_path         # e.g. /dev/video0
string pipeline_state      # NULL | READY | PAUSED | PLAYING | ERROR
float32 fps                # Measured output FPS
bool streaming             # True if pipeline is active
uint32 frame_count         # Total frames sent
string error_msg           # Last error (empty if ok)
int32 brightness_level     # Current V4L2 brightness
int32 exposure_time        # Current exposure value
bool  auto_exposure_on     # Current exposure mode

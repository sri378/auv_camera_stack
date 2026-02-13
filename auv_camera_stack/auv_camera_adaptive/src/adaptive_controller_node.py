import rclpy
from rclpy.node import Node
from std_msgs.msg import Float32
from auv_camera_msgs.msg import CameraControl

class AdaptiveControllerNode(Node):

    def __init__(self):
        super().__init__('adaptive_controller')

        # --- Parameters ---
        self.declare_parameter('target_brightness',   100.0)  # 0-255
        self.declare_parameter('brightness_deadband',  10.0)  # ± this = no change
        self.declare_parameter('kp',                    0.5)  # proportional gain
        self.declare_parameter('ki',                    0.05) # integral gain
        self.declare_parameter('min_exposure',          50)   # minimum safe value
        self.declare_parameter('max_exposure',         800)   # max before whiteout risk
        self.declare_parameter('whiteout_threshold',   210.0) # brightness = whiteout
        self.declare_parameter('whiteout_frames',        3)   # consecutive frames
        self.declare_parameter('whiteout_exposure',     30)   # failsafe exposure
        self.declare_parameter('control_hz',             2.0) # control loop rate
        self.declare_parameter('enabled',              True)  # can disable from CLI

        # Baseline control state 
        self.current_exposure  = 200
        self.current_saturation = 70
        self.current_gain       = 15
        self.integral_error     = 0.0
        self.whiteout_count     = 0
        self.in_whiteout_mode   = False
        self.last_brightness    = 0.0

        # --- Subscriber: brightness from receiver ---
        self.brightness_sub = self.create_subscription(
            Float32, '/camera/brightness',
            self.on_brightness, 10)

        # --- Publisher: camera control to Jetson ---
        self.ctrl_pub = self.create_publisher(
            CameraControl, '/camera/control', 10)

        # --- Control loop timer ---
        hz = self.get_parameter('control_hz').value
        self.create_timer(1.0 / hz, self.control_loop)

        self.get_logger().info(
            f'AdaptiveController started (target={self.get_parameter("target_brightness").value})')

    def on_brightness(self, msg: Float32):
        self.last_brightness = msg.data

    def control_loop(self):
        if not self.get_parameter('enabled').value:
            return

        brightness = self.last_brightness
        if brightness == 0.0:
            return  # No data yet

        target    = self.get_parameter('target_brightness').value
        deadband  = self.get_parameter('brightness_deadband').value
        kp        = self.get_parameter('kp').value
        ki        = self.get_parameter('ki').value
        min_exp   = self.get_parameter('min_exposure').value
        max_exp   = self.get_parameter('max_exposure').value
        wo_thresh = self.get_parameter('whiteout_threshold').value
        wo_frames = self.get_parameter('whiteout_frames').value
        wo_exp    = self.get_parameter('whiteout_exposure').value
        # 1. Whiteout detection
        if brightness > wo_thresh:
            self.whiteout_count += 1
        else:
            self.whiteout_count = max(0, self.whiteout_count - 1)

        if self.whiteout_count >= wo_frames:
            if not self.in_whiteout_mode:
                self.get_logger().warning(
                    f'⚠ WHITEOUT detected (brightness={brightness:.1f})! '
                    f'Emergency exposure cut → {wo_exp}')
                self.in_whiteout_mode = True
                self.integral_error   = 0.0  # reset integrator

            self.current_exposure = wo_exp
            self.publish_control()
            return

        # Recovery from whiteout
        if self.in_whiteout_mode and brightness < wo_thresh - 20:
            self.get_logger().info('Whiteout recovered, resuming normal control')
            self.in_whiteout_mode = False
        # 2. PI controller
        error = target - brightness

        # Deadband: don't chase small fluctuations
        if abs(error) < deadband:
            return

        self.integral_error += error

        # Anti-windup: clamp integral
        max_i = (max_exp - min_exp) / max(ki, 1e-6)
        self.integral_error = max(-max_i, min(max_i, self.integral_error))

        # Control output (in exposure units)
        delta = kp * error + ki * self.integral_error

        new_exposure = int(self.current_exposure + delta)
        new_exposure = max(min_exp, min(max_exp, new_exposure))

        if new_exposure != self.current_exposure:
            self.get_logger().debug(
                f'Brightness={brightness:.1f} err={error:.1f} '
                f'exp: {self.current_exposure} → {new_exposure}')
            self.current_exposure = new_exposure
            self.publish_control()

    def publish_control(self):
        """Build and send CameraControl with current adaptive settings."""
        msg = CameraControl()

        # Always manual — never give the camera autonomy over exposure
        msg.auto_exposure      = False
        msg.exposure_time      = self.current_exposure
        msg.white_balance_auto = False
        msg.white_balance_temp = 5500   # Fixed: neutral-cool for water

        msg.brightness         = 0
        msg.contrast           = 42
        msg.saturation         = self.current_saturation
        msg.hue                = 6
        msg.gamma              = 100
        msg.gain               = self.current_gain
        msg.sharpness          = 3
        msg.backlight_comp     = 0      # Always OFF underwater
        msg.power_line_freq    = 1      # 50 Hz

        self.ctrl_pub.publish(msg)


def main(args=None):
    rclpy.init(args=args)
    node = AdaptiveControllerNode()
    rclpy.spin(node)
    rclpy.shutdown()


if __name__ == '__main__':
    main()

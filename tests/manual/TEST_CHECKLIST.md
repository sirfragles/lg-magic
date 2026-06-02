# Hardware-in-the-Loop Test Checklist

> **Prerequisites**: LG Magic Remote (MR20), Bluetooth adapter, Linux system with kernel 4.15+

---

## 1. Build and Load

- [ ] Build kernel module: `cd kernel && make`
- [ ] Load module: `sudo insmod lg_magic.ko`
- [ ] Verify load in dmesg: `dmesg | grep lgmagic`
- [ ] Verify module in lsmod: `lsmod | grep lg_magic`

## 2. Device Detection

- [ ] Pair remote with Bluetooth (system settings or bluetoothctl)
- [ ] Verify HIDRAW device created: `ls /dev/hidraw*`
- [ ] Verify input devices: `ls /dev/input/by-id/ | grep -i lg`
- [ ] Verify evdev device names:
  ```bash
  for dev in /dev/input/event*; do
    evtest --info "$dev" 2>/dev/null | grep -B1 "LG Magic"
  done
  ```

## 3. Button Verification

Load module with debug: `sudo rmmod lg_magic && sudo insmod lg_magic.ko debug=2`

For each button, press and verify the keycode in dmesg or evtest:

### Power
- [ ] POWER (top-right) → KEY_POWER
- [ ] SLEEP → KEY_SLEEP

### Numbers
- [ ] 0-9 → KEY_0 through KEY_9

### Center
- [ ] OK (center press) → KEY_ENTER (nav mode) / BTN_LEFT (airmouse mode)
- [ ] Wheel scroll up → KEY_UP (nav mode) / REL_WHEEL positive (airmouse)
- [ ] Wheel scroll down → KEY_DOWN (nav mode) / REL_WHEEL negative (airmouse)

### Navigation
- [ ] UP → KEY_UP
- [ ] DOWN → KEY_DOWN
- [ ] LEFT → KEY_LEFT
- [ ] RIGHT → KEY_RIGHT

### Volume / Audio
- [ ] VOL+ → KEY_VOLUMEUP
- [ ] VOL- → KEY_VOLUMEDOWN
- [ ] MUTE → KEY_MUTE
- [ ] MIC (voice/assistant) → KEY_VOICECOMMAND

### Home / Navigation
- [ ] HOME (house icon) → KEY_HOME
- [ ] BACK (arrow) → KEY_BACK
- [ ] SETTINGS (gear) → KEY_SETUP
- [ ] GUIDE → KEY_PROGRAM

### Media / TV
- [ ] LIST → KEY_LIST
- [ ] ... (more options) → KEY_MENU
- [ ] IVI / Prime Video → KEY_MEDIA
- [ ] TV / INPUT → KEY_TV
- [ ] STB MENU → KEY_CONTEXT_MENU
- [ ] MOVIES → KEY_VIDEO

### Channel
- [ ] CH- → KEY_CHANNELDOWN

### Playback
- [ ] PLAY → KEY_PLAY
- [ ] PAUSE → KEY_PAUSE

### Color Buttons
- [ ] RED → KEY_RED
- [ ] GREEN → KEY_GREEN
- [ ] YELLOW → KEY_YELLOW
- [ ] BLUE → KEY_BLUE

## 4. IMU Data

- [ ] Load with IMU evdev: `sudo rmmod lg_magic && sudo insmod lg_magic.ko imu_evdev=1 debug=2`
- [ ] Verify IMU device appears: `evtest` should list "LG Magic Remote IMU"
- [ ] Open evtest on IMU device, move remote → verify ABS_X/Y/Z and ABS_RX/RY/RZ change
- [ ] Verify MSC_SERIAL counter increments

## 5. Airmouse

- [ ] Place calibration file: `sudo cp lg_magic_calib.bin /lib/firmware/`
- [ ] Load with airmouse: `sudo rmmod lg_magic && sudo insmod lg_magic.ko airmouse=1 imu_evdev=1 debug=2`
- [ ] Check dmesg for "Loaded calibration from lg_magic_calib.bin"
- [ ] Move remote quickly — verify REL_X/REL_Y events in evtest
- [ ] Press navigation button after airmouse mode → should return to nav mode
- [ ] Wheel in airmouse mode → REL_WHEEL events (not KEY_UP/DOWN)

### Airmouse Tuning
- [ ] Change threshold: `echo 200 | sudo tee /sys/module/lg_magic/parameters/airmouse_threshold`
- [ ] Move remote — verify easier airmouse activation (lower threshold)
- [ ] Change threshold back: `echo 500` — verify harder activation
- [ ] Disable/enable airmouse: `echo 0 | sudo tee /sys/module/lg_magic/parameters/airmouse` → verify no REL events

## 6. Suspend/Resume

- [ ] With module loaded and remote paired, suspend system: `sudo systemctl suspend`
- [ ] Wake system
- [ ] Verify dmesg shows suspend/resume logs (debug=2)
- [ ] Verify remote still works: press any button, check evtest
- [ ] Verify airmouse still works after resume

## 7. Module Unload

- [ ] Press and hold a button on remote (e.g., OK)
- [ ] While holding, unload module: `sudo rmmod lg_magic`
- [ ] Verify no kernel oops or stuck key in dmesg
- [ ] Reload module and verify clean state

## 8. Python Tools

- [ ] `python3 display_imu.py` — verify IMU data streaming
- [ ] `python3 display_imu.py --csv test.csv` — verify CSV output format
- [ ] `python3 display_imu.py --calib calib.json --ahrs` — verify Euler angles
- [ ] `python3 display_imu.py --calib calib.json --cube` — verify 3D cube renders
- [ ] `python3 display_imu.py --calib calib.json --mouse` — verify virtual mouse (requires uinput)
- [ ] `python3 lg_magic.py` — verify HIDRAW packet analyzer works (auto-detect)

## 9. Calibration Round-Trip

- [ ] Collect gyro samples: `python3 display_imu.py --csv gyro_samples.csv` (hold remote stationary)
- [ ] Collect accel samples: `python3 display_imu.py --csv accel_samples.csv` (rotate through 6 orientations)
- [ ] Calibrate: `python3 calibrate.py --gyro gyro_samples.csv calib.json`
- [ ] Calibrate: `python3 calibrate.py --accel accel_samples.csv calib.json` (auto-merges)
- [ ] Convert: `python3 convert_calib.py calib.json lg_magic_calib.bin --alpha 0.2 --mouse_k 0.5`
- [ ] Install: `sudo cp lg_magic_calib.bin /lib/firmware/`
- [ ] Reload module, verify calibration loads from dmesg

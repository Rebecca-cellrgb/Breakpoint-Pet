# Firmware Source

This directory contains the Breakpoint Pet ESP-IDF application layer and hardware integration scripts.

## Contents

- `apps/main/`: main application, checkpoint state machine, voice, cloud, camera, pose, and peripheral code.
- `apps/sdkconfig.defaults`: default build configuration.
- `apps/partitions.csv`: flash partition table, including the pose model partition.
- `pose_test/`: PC pose recognition and camera integration scripts.

## External Dependencies

The full ESP-IDF, QUMIOS, Agora IoT SDK, and ESP-ADF framework are intentionally not copied here. Paths such as `QDEV` and `AGORA_SDK` in `apps/main/CMakeLists.txt` must point to the matching SDKs on the build machine. Component dependencies are declared in `apps/main/idf_component.yml`.

Virtual environments, build output, managed components, logs, and downloaded test data are excluded from this submission package.

# SenseCAP Watcher

## Introduction

The project provides custom firmware for the SenseCAP Watcher that makes it behave more like an offline device.

Currently, the features include:
- Being able to take pictures and save them to the SD card
- A Flashlight
- An idle animation

This code might not be the cleanest as I barely comprehend the SenseCap Device Setup, but it runs and is stable on my unit.

## Getting Started

### Install ESP IDF

Follow instructions in this guide
[ESP-IDF - Get Started](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html)
to setup the built toolchain used by SSCMA examples. Currently we're using the latest version `v5.2.1`.

### Clone and Setup the Repository

1. Clone our repository.

    ```sh
    git clone https://github.com/Seeed-Studio/SenseCAP-Watcher
    ```

2. Go to `SenseCAP-Watcher` folder.

    ```sh
    cd SenseCAP-Watcher
    ```

3. Generate build config using ESP-IDF.

    ```sh
    # set build target
    idf.py set-target esp32s3
    ```

4. Build the firmware.

    ```sh
    idf.py build
    ```

5. Flash the firmware to device and Run.

- Use `Ctrl+]` to exit monitor.

    ```sh
    idf.py --port /dev/ttyACM0 flash monitor
    ```

## License

```
This project is released under the Apache 2.0 license.
```


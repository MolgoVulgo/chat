import os
import subprocess

Import("env")

platform = env.PioPlatform()


def build_ota_image(source, target, env):
    elf_path = env.subst("$BUILD_DIR/${PROGNAME}.elf")
    output_path = env.subst("$BUILD_DIR/${PROGNAME}.ota.bin")
    esptool = os.path.join(platform.get_package_dir("tool-esptoolpy"), "esptool.py")
    python = env.subst("$PYTHONEXE")

    cmd = [
        python,
        esptool,
        "elf2image",
        "--version",
        "2",
        "--flash_mode",
        "dio",
        "--flash_freq",
        "40m",
        "--flash_size",
        "16MB",
        "-o",
        output_path,
        elf_path,
    ]

    print("Building OTA image %s" % output_path)
    subprocess.check_call(cmd)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", build_ota_image)

"""Regression checks for the generated SDL configuration guard; no audio device needed."""

import pathlib
import subprocess
import tempfile
import unittest

GUARD = pathlib.Path(__file__).resolve().parents[1] / "native/monogame/ValidateLinuxSdlAudio.cmake"


class LinuxSdlAudioTests(unittest.TestCase):
    def check_configuration(self, contents=None, duplicate=False):
        with tempfile.TemporaryDirectory(prefix="monogame-sdl-audio-") as directory:
            root = pathlib.Path(directory)
            if contents is not None:
                header = root / "include-config-/SDL2/SDL_config.h"
                header.parent.mkdir(parents=True)
                header.write_text(contents)
                if duplicate:
                    other = root / "include/SDL2/SDL_config.h"
                    other.parent.mkdir(parents=True)
                    other.write_text(contents)
            return subprocess.run(
                ["cmake", f"-DSDL_BUILD_DIR={root}", "-P", str(GUARD)],
                text=True, capture_output=True, check=False,
            )

    def test_required_backends_pass_without_pipewire(self):
        result = self.check_configuration("#define SDL_AUDIO_DRIVER_ALSA 1\n#define SDL_AUDIO_DRIVER_PULSEAUDIO 1\n")
        self.assertEqual(0, result.returncode, result.stderr)

    def test_missing_pulse_fails_with_dependency_guidance(self):
        result = self.check_configuration("#define SDL_AUDIO_DRIVER_ALSA 1\n/* #undef SDL_AUDIO_DRIVER_PULSEAUDIO */\n")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("PULSEAUDIO", result.stderr)
        self.assertIn("libpulse-dev", result.stderr)

    def test_missing_alsa_fails(self):
        result = self.check_configuration("#define SDL_AUDIO_DRIVER_PULSEAUDIO 1\n")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("ALSA", result.stderr)

    def test_disabled_or_similarly_named_defines_do_not_pass(self):
        for definition in ["#define SDL_AUDIO_DRIVER_ALSA 0", "#define SDL_AUDIO_DRIVER_ALSA 10", "#define SDL_AUDIO_DRIVER_ALSA_EXTRA 1", "/* #define SDL_AUDIO_DRIVER_ALSA 1 */"]:
            with self.subTest(definition=definition):
                result = self.check_configuration(definition + "\n#define SDL_AUDIO_DRIVER_PULSEAUDIO 1\n")
                self.assertNotEqual(0, result.returncode)

    def test_dummy_and_disk_only_fail(self):
        result = self.check_configuration("#define SDL_AUDIO_DRIVER_DISK 1\n#define SDL_AUDIO_DRIVER_DUMMY 1\n")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("ALSA, PULSEAUDIO", result.stderr)

    def test_unconfigured_build_fails(self):
        result = self.check_configuration()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("Configure bundled SDL2 first", result.stderr)

    def test_ambiguous_configuration_fails(self):
        result = self.check_configuration("#define SDL_AUDIO_DRIVER_ALSA 1\n#define SDL_AUDIO_DRIVER_PULSEAUDIO 1\n", duplicate=True)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("found 2", result.stderr)


if __name__ == "__main__":
    unittest.main()

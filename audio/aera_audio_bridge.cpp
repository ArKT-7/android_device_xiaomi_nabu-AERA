/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Narrow plugin-to-TinyALSA PCM bridge for Xiaomi Pad 5 (nabu) recovery.
 * Untrusted plugins never receive access to ALSA, Binder, stock partitions, or
 * this process. Only the dedicated sandboxed plugin UIDs may connect and send
 * fixed-format audio over one abstract local socket.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace {

constexpr uid_t kBrowserUid = 99090;
constexpr uid_t kTelegramUid = 99091;
constexpr uid_t kMediaUid = 99092;
constexpr uid_t kDoomUid = 99094;

constexpr uint32_t kMagic = 0x41525041U;
constexpr uint32_t kRate = 48000U;
constexpr uint32_t kChannels = 2U;
constexpr uint32_t kBits = 16U;
constexpr unsigned int kPeriodFrames = 1024U;
constexpr size_t kPeriodSamples = kPeriodFrames * kChannels;
constexpr size_t kFrameBytes = kChannels * (kBits / 8U);
constexpr size_t kPeriodBytes = kPeriodFrames * kFrameBytes;
constexpr size_t kMaxClients = 4U;

constexpr int kIdleTimeoutMs = 1500;
constexpr int kHandshakeTimeoutMs = 1000;
constexpr int kDefaultVolumePercent = 80;
constexpr int kLeftTdmSlot = 4;
constexpr int kRightTdmSlot = 5;

constexpr char kSocketName[] = "aera-browser-audio-v1";
constexpr char kBootstrap[] = "/system/bin/aera-audio-bootstrap";
constexpr char kVolumePath[] = "/tmp/aera-audio-volume";
constexpr char kRotationPath[] = "/tmp/aera-audio-rotation";
constexpr const char* kSpeakers[] = {"TR", "TL", "BR", "BL"};

volatile sig_atomic_t g_stop = 0;

void Stop(int) { g_stop = 1; }

int64_t MonotonicMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool IsAllowedUid(uid_t uid) {
  return uid == kBrowserUid || uid == kTelegramUid || uid == kMediaUid ||
         uid == kDoomUid;
}

int ReadSmallIntFile(const char* path, int fallback) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return fallback;
  char value[16]{};
  const ssize_t count = read(fd, value, sizeof(value) - 1);
  close(fd);
  if (count <= 0) return fallback;
  return atoi(value);
}

int ReadVolumePercent() {
  return std::clamp(ReadSmallIntFile(kVolumePath, kDefaultVolumePercent), 0, 100);
}

int ReadRotationDegrees() {
  const int rot = ReadSmallIntFile(kRotationPath, 0);
  return (rot == 90 || rot == 180 || rot == 270) ? rot : 0;
}

// matches AERA visible screen touch/scanout orientation on Xiaomi Pad 5:
//   0   (Portrait):          Left = TL, BL | Right = TR, BR
//   270 (GPU Landscape):     Left = TL, TR | Right = BL, BR
//   90  (CPU Landscape):     Left = BL, BR | Right = TL, TR
//   180 (Reverse Portrait):  Left = TR, BR | Right = TL, BL
bool IsRightSpeaker(const char* spk, int rotation) {
  switch (rotation) {
    case 90:
      return strcmp(spk, "TL") == 0 || strcmp(spk, "TR") == 0;
    case 180:
      return strcmp(spk, "TL") == 0 || strcmp(spk, "BL") == 0;
    case 270:
      return strcmp(spk, "BL") == 0 || strcmp(spk, "BR") == 0;
    case 0:
    default:
      return strcmp(spk, "TR") == 0 || strcmp(spk, "BR") == 0;
  }
}

bool StartBackend() {
  const pid_t child = fork();
  if (child < 0) return false;
  if (!child) {
    execl(kBootstrap, "aera-audio-bootstrap", nullptr);
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return false;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool HasMixerControl(mixer* card, const char* name) {
  return mixer_get_ctl_by_name(card, name) != nullptr;
}

bool SetMixerValue(mixer* card, const char* name, int value) {
  mixer_ctl* control = mixer_get_ctl_by_name(card, name);
  if (control == nullptr) {
    fprintf(stderr, "AERA audio: mixer control '%s' is missing\n", name);
    return false;
  }
  const unsigned int count = mixer_ctl_get_num_values(control);
  for (unsigned int index = 0; index < count; ++index) {
    if (mixer_ctl_set_value(control, index, value) != 0) {
      fprintf(stderr, "AERA audio: could not set mixer control '%s'\n", name);
      return false;
    }
  }
  return true;
}

bool SetMixerEnum(mixer* card, const char* name, const char* value) {
  mixer_ctl* control = mixer_get_ctl_by_name(card, name);
  if (control == nullptr) {
    fprintf(stderr, "AERA audio: mixer control '%s' is missing\n", name);
    return false;
  }
  if (mixer_ctl_set_enum_by_string(control, value) != 0) {
    fprintf(stderr, "AERA audio: could not set mixer control '%s' to '%s'\n",
            name, value);
    return false;
  }
  return true;
}

bool SetChannelSwapControl(mixer* card, const char* spk, bool is_right) {
  const char* candidates[] = {
      "%s Channel Swap",
      "%s DSP1 Protection cd Channel Swap",
      "%s Protection Channel Swap",
  };
  for (const char* pattern : candidates) {
    char name[64];
    snprintf(name, sizeof(name), pattern, spk);
    mixer_ctl* control = mixer_get_ctl_by_name(card, name);
    if (control == nullptr) continue;

    switch (mixer_ctl_get_type(control)) {
      case MIXER_CTL_TYPE_ENUM:
        return mixer_ctl_set_enum_by_string(control, is_right ? "On" : "Off") == 0;
      case MIXER_CTL_TYPE_BOOL:
      case MIXER_CTL_TYPE_INT:
        return mixer_ctl_set_value(control, 0, is_right ? 1 : 0) == 0;
      case MIXER_CTL_TYPE_BYTE:
        if (mixer_ctl_get_num_values(control) == 4U) {
          const uint8_t word[4] = {0, 0, 0, static_cast<uint8_t>(is_right ? 1 : 0)};
          return mixer_ctl_set_array(control, word, sizeof(word)) == 0;
        }
        break;
      default:
        break;
    }
  }
  return false;
}

bool IsDspSourceActive(mixer* card, const char* spk) {
  char name[64];
  snprintf(name, sizeof(name), "%s PCM Source", spk);
  mixer_ctl* control = mixer_get_ctl_by_name(card, name);
  if (control == nullptr || mixer_ctl_get_type(control) != MIXER_CTL_TYPE_ENUM) {
    return false;
  }
  const int item = mixer_ctl_get_value(control, 0);
  if (item < 0) return false;
  const char* current =
      mixer_ctl_get_enum_string(control, static_cast<unsigned int>(item));
  return current != nullptr && strcmp(current, "DSP") == 0;
}

void ApplySpeakerRotationOnCard(mixer* card, int rotation) {
  for (const char* spk : kSpeakers) {
    char name[64];
    const bool is_right = IsRightSpeaker(spk, rotation);
    const bool dsp_active = IsDspSourceActive(card, spk);

    snprintf(name, sizeof(name), "%s ASPRX1 Slot Position", spk);
    SetMixerValue(card, name,
                  (!dsp_active && is_right) ? kRightTdmSlot : kLeftTdmSlot);

    snprintf(name, sizeof(name), "%s ASPRX2 Slot Position", spk);
    SetMixerValue(card, name, kRightTdmSlot);

    if (dsp_active) {
      SetChannelSwapControl(card, spk, is_right);
    }
  }
}

void ApplySpeakerRotation(int rotation) {
  mixer* card = mixer_open(0);
  if (card == nullptr) return;
  mixer_add_new_ctls(card);
  ApplySpeakerRotationOnCard(card, rotation);
  mixer_close(card);
}

bool PrepareMixer() {
  mixer* card = mixer_open(0);
  if (card == nullptr) {
    fprintf(stderr, "AERA audio: could not open sound card 0 mixer\n");
    return false;
  }

  // configure Quaternary TDM backend format befoer connecting MultiMedia1 FE
  SetMixerEnum(card, "QUAT_TDM_RX_0 SampleRate", "KHZ_48");
  SetMixerEnum(card, "QUAT_TDM_RX_0 Channels", "Two");
  SetMixerEnum(card, "QUAT_TDM_RX_0 Format", "S24_LE");
  const bool fe_ready =
      SetMixerValue(card, "QUAT_TDM_RX_0 Audio Mixer MultiMedia1", 1);

  // trigger Halo Core DSP firmware preload across all 4 CS35L41 amps in parallel
  bool any_dsp_preloaded = false;
  bool dsp_ready[4]{};
  for (size_t i = 0; i < 4U; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "%s DSP1 Firmware", kSpeakers[i]);
    if (HasMixerControl(card, name) && SetMixerEnum(card, name, "Protection")) {
      snprintf(name, sizeof(name), "%s DSP1 Preload Switch", kSpeakers[i]);
      if (HasMixerControl(card, name) && SetMixerValue(card, name, 1)) {
        dsp_ready[i] = true;
        any_dsp_preloaded = true;
      }
    }
  }

  // wm_adsp loads .wmfw/.bin on an async kernel workqueue and dynamically adds
  // DSP algorithm mixer controls (such as Channel Swap) once preload completes.
  if (any_dsp_preloaded) {
    for (int wait_step = 0; wait_step < 12; ++wait_step) {
      mixer_add_new_ctls(card);
      if (HasMixerControl(card, "TR Channel Swap") ||
          HasMixerControl(card, "TR DSP1 Protection cd Channel Swap")) {
        break;
      }
      usleep(25000);
    }
    mixer_add_new_ctls(card);
  }

  // configure power stage, Class-H boost, DRE, gains, and stereo slots
  const int rotation = ReadRotationDegrees();
  int configured_amps = 0;

  for (size_t i = 0; i < 4U; ++i) {
    const char* spk = kSpeakers[i];
    char name[64];

    snprintf(name, sizeof(name), "%s PCM Source", spk);
    if (!dsp_ready[i] || !SetMixerEnum(card, name, "DSP")) {
      SetMixerEnum(card, name, "ASP");
    }

    snprintf(name, sizeof(name), "%s ASPTX Ref", spk);
    if (HasMixerControl(card, name)) {
      SetMixerEnum(card, name, "Ref");
    }

    bool amp_ok = false;
    const char* amp_switches[] = {
        "%s AMP Enable",
        "%s Main AMP Enable Switch",
        "%s AMP Enable Switch",
    };
    for (const char* pattern : amp_switches) {
      snprintf(name, sizeof(name), pattern, spk);
      if (HasMixerControl(card, name)) {
        amp_ok = SetMixerValue(card, name, 1);
        break;
      }
    }
    if (amp_ok) ++configured_amps;

    snprintf(name, sizeof(name), "%s Boost Converter Enable", spk);
    if (HasMixerControl(card, name)) SetMixerValue(card, name, 2);

    snprintf(name, sizeof(name), "%s Boost Class-H Tracking Enable", spk);
    if (HasMixerControl(card, name)) SetMixerValue(card, name, 1);

    const char* dre_switches[] = {"%s DRE DRE Switch", "%s DRE Switch"};
    for (const char* pattern : dre_switches) {
      snprintf(name, sizeof(name), pattern, spk);
      if (HasMixerControl(card, name)) {
        SetMixerValue(card, name, 1);
        break;
      }
    }

    snprintf(name, sizeof(name), "%s PCM Soft Ramp", spk);
    if (HasMixerControl(card, name)) SetMixerEnum(card, name, "4ms");

    // 817 = 0 dB unity digital volume on CS35L41, 17 analog gain keeps safe
    // headroom across 4 uncalibrated micro-speakers while audible at AERA 30% default
    snprintf(name, sizeof(name), "%s Digital PCM Volume", spk);
    SetMixerValue(card, name, 817);

    snprintf(name, sizeof(name), "%s AMP PCM Gain", spk);
    SetMixerValue(card, name, 17);
  }

  ApplySpeakerRotationOnCard(card, rotation);
  mixer_close(card);

  const bool ok = fe_ready && configured_amps == 4;
  fprintf(stderr,
          "AERA audio: Xiaomi Pad 5 quad-speaker mixer configured "
          "(fe=%d, amps=%d/4, rot=%d)\n",
          fe_ready ? 1 : 0, configured_amps, rotation);
  return ok;
}

pcm* OpenPcm() {
  pcm_config config{};
  config.channels = kChannels;
  config.rate = kRate;
  config.period_size = kPeriodFrames;
  config.period_count = 4;
  config.format = PCM_FORMAT_S16_LE;
  config.start_threshold = config.period_size * 2;
  config.stop_threshold = config.period_size * config.period_count;
  config.silence_threshold = config.stop_threshold;

  pcm* output = pcm_open(0, 0, PCM_OUT | PCM_NORESTART, &config);
  if (output == nullptr || !pcm_is_ready(output)) {
    fprintf(stderr, "AERA audio: PCM output is unavailable: %s\n",
            output == nullptr ? "open failed" : pcm_get_error(output));
    if (output != nullptr) pcm_close(output);
    return nullptr;
  }
  return output;
}

bool WritePcm(pcm*& output, const uint8_t* data, size_t size) {
  size_t offset = 0;
  bool restarted = false;
  while (offset < size) {
    const unsigned int frames =
        static_cast<unsigned int>((size - offset) / kFrameBytes);
    const int written = pcm_writei(output, data + offset, frames);
    if (written <= 0) {
      const int error = errno;
      if (!restarted && (error == EPIPE || error == ESTRPIPE)) {
        // Re-prepare alone leaves this Qualcomm DSP stream stuck after an XRUN.
        fprintf(stderr, "AERA audio: reopening PCM after underrun\n");
        pcm_close(output);
        output = OpenPcm();
        if (output == nullptr) return false;
        restarted = true;
        continue;
      }
      fprintf(stderr, "AERA audio: PCM write failed: %s\n",
              pcm_get_error(output));
      return false;
    }
    offset += static_cast<size_t>(written) * kFrameBytes;
  }
  return true;
}

struct ClientSlot {
  int fd = -1;
  size_t hello_filled = 0;
  std::array<uint8_t, 16> hello_bytes{};
  size_t period_filled = 0;
  std::array<int16_t, kPeriodSamples> samples{};
  bool has_ready_period = false;
  int64_t connected_at_ms = 0;
  int64_t last_read_ms = 0;
};

void CloseSlot(ClientSlot& slot) {
  if (slot.fd >= 0) {
    close(slot.fd);
    slot.fd = -1;
  }
  slot.hello_filled = 0;
  slot.period_filled = 0;
}

void FlushPartialPeriod(ClientSlot& slot) {
  if (slot.period_filled == 0) return;
  auto* raw = reinterpret_cast<uint8_t*>(slot.samples.data());
  memset(raw + slot.period_filled, 0, kPeriodBytes - slot.period_filled);
  slot.period_filled = 0;
  slot.has_ready_period = true;
}

void RunBridgeServer(int server) {
  std::array<ClientSlot, kMaxClients> clients{};
  pcm* output = nullptr;
  int64_t last_audio_ms = 0;
  unsigned int period_counter = 0;
  int volume = ReadVolumePercent();
  int current_rotation = -1;

  while (!g_stop) {
    std::array<pollfd, 1 + kMaxClients> pfds{};
    std::array<int, kMaxClients> client_pfd_idx{};
    client_pfd_idx.fill(-1);
    size_t pfd_count = 0;

    bool has_free_slot = false;
    bool any_ready_period = false;
    bool any_pending_handshake = false;
    bool any_partial_period = false;

    for (const ClientSlot& slot : clients) {
      if (slot.fd < 0 && !slot.has_ready_period) has_free_slot = true;
      if (slot.has_ready_period) any_ready_period = true;
      if (slot.fd >= 0 && slot.hello_filled < slot.hello_bytes.size()) {
        any_pending_handshake = true;
      }
      if (slot.fd >= 0 && slot.period_filled > 0) any_partial_period = true;
    }

    int server_pfd_idx = -1;
    if (has_free_slot) {
      server_pfd_idx = static_cast<int>(pfd_count);
      pfds[pfd_count++] = pollfd{server, POLLIN, 0};
    }

    for (size_t i = 0; i < kMaxClients; ++i) {
      if (clients[i].fd >= 0 && !clients[i].has_ready_period) {
        client_pfd_idx[i] = static_cast<int>(pfd_count);
        pfds[pfd_count++] = pollfd{clients[i].fd, POLLIN, 0};
      }
    }

    int timeout_ms = -1;
    if (any_ready_period) {
      timeout_ms = 0;
    } else if (any_pending_handshake || any_partial_period) {
      timeout_ms = 100;
    } else if (output != nullptr) {
      const int64_t elapsed = MonotonicMs() - last_audio_ms;
      timeout_ms = static_cast<int>(
          std::clamp<int64_t>(kIdleTimeoutMs - elapsed, 1, kIdleTimeoutMs));
    }

    const int ready = poll(pfds.data(), static_cast<nfds_t>(pfd_count), timeout_ms);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }

    int64_t now_ms = MonotonicMs();

    // accept incoming plugin connections without blocking active playback
    if (server_pfd_idx >= 0 &&
        (pfds[static_cast<size_t>(server_pfd_idx)].revents & POLLIN)) {
      const int client_fd =
          accept4(server, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (client_fd >= 0) {
        ucred credentials{};
        socklen_t credentials_size = sizeof(credentials);
        if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &credentials,
                       &credentials_size) != 0 ||
            credentials_size != sizeof(credentials) ||
            !IsAllowedUid(credentials.uid)) {
          fprintf(stderr, "AERA audio: rejected local UID %u\n", credentials.uid);
          close(client_fd);
        } else {
          bool assigned = false;
          for (ClientSlot& slot : clients) {
            if (slot.fd < 0 && !slot.has_ready_period) {
              slot.fd = client_fd;
              slot.hello_filled = 0;
              slot.period_filled = 0;
              slot.connected_at_ms = now_ms;
              slot.last_read_ms = now_ms;
              assigned = true;
              break;
            }
          }
          if (!assigned) close(client_fd);
        }
      }
    }

    // better non-blocking handshakes and read complete DSP periods
    for (size_t i = 0; i < kMaxClients; ++i) {
      ClientSlot& slot = clients[i];
      if (slot.fd < 0) continue;

      const int idx = client_pfd_idx[i];
      const short revents =
          idx >= 0 ? pfds[static_cast<size_t>(idx)].revents : 0;

      if (revents & (POLLIN | POLLHUP | POLLERR)) {
        if (slot.hello_filled < slot.hello_bytes.size()) {
          while (slot.hello_filled < slot.hello_bytes.size()) {
            const ssize_t n = read(slot.fd,
                                   slot.hello_bytes.data() + slot.hello_filled,
                                   slot.hello_bytes.size() - slot.hello_filled);
            if (n > 0) {
              slot.hello_filled += static_cast<size_t>(n);
              slot.last_read_ms = now_ms;
              continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            CloseSlot(slot);
            break;
          }

          if (slot.fd >= 0 && slot.hello_filled == slot.hello_bytes.size()) {
            uint32_t hello[4]{};
            memcpy(hello, slot.hello_bytes.data(), sizeof(hello));
            if (hello[0] != kMagic || hello[1] != kRate ||
                hello[2] != kChannels || hello[3] != kBits) {
              fprintf(stderr, "AERA audio: rejected invalid PCM handshake\n");
              CloseSlot(slot);
            }
          }
        }

        if (slot.fd >= 0 && slot.hello_filled == slot.hello_bytes.size() &&
            !slot.has_ready_period) {
          auto* raw = reinterpret_cast<uint8_t*>(slot.samples.data());
          while (slot.period_filled < kPeriodBytes) {
            const ssize_t n = read(slot.fd, raw + slot.period_filled,
                                   kPeriodBytes - slot.period_filled);
            if (n > 0) {
              slot.period_filled += static_cast<size_t>(n);
              slot.last_read_ms = now_ms;
              continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            FlushPartialPeriod(slot);
            CloseSlot(slot);
            break;
          }
          if (slot.period_filled == kPeriodBytes) {
            slot.period_filled = 0;
            slot.has_ready_period = true;
          }
        }
      }

      if (slot.fd >= 0 && slot.hello_filled < slot.hello_bytes.size() &&
          (now_ms - slot.connected_at_ms) > kHandshakeTimeoutMs) {
        CloseSlot(slot);
      }

      if (slot.fd >= 0 && slot.period_filled > 0 &&
          (now_ms - slot.last_read_ms) >= 120) {
        FlushPartialPeriod(slot);
      }
    }

    // miix/scale ready periods and write to Qualcomm PCM backend
    size_t ready_streams = 0;
    size_t single_idx = 0;
    for (size_t i = 0; i < kMaxClients; ++i) {
      if (clients[i].has_ready_period) {
        single_idx = i;
        ++ready_streams;
      }
    }

    if (ready_streams > 0) {
      if (output == nullptr || (period_counter++ % 12U) == 0) {
        volume = ReadVolumePercent();
        const int rot = ReadRotationDegrees();
        if (rot != current_rotation) {
          ApplySpeakerRotation(rot);
          current_rotation = rot;
        }
      }

      if (output == nullptr) {
        output = OpenPcm();
      }

      if (output == nullptr) {
        for (ClientSlot& slot : clients) {
          slot.has_ready_period = false;
        }
        usleep(50000);
        continue;
      }

      std::array<int16_t, kPeriodSamples> out_samples{};
      if (ready_streams == 1U) {
        const auto& src = clients[single_idx].samples;
        for (size_t s = 0; s < kPeriodSamples; ++s) {
          out_samples[s] = static_cast<int16_t>(
              (static_cast<int32_t>(src[s]) * volume) / 100);
        }
        clients[single_idx].has_ready_period = false;
      } else {
        for (size_t s = 0; s < kPeriodSamples; ++s) {
          int32_t mixed = 0;
          for (ClientSlot& slot : clients) {
            if (slot.has_ready_period) {
              mixed += slot.samples[s];
            }
          }
          mixed = std::clamp(mixed, -32768, 32767);
          out_samples[s] = static_cast<int16_t>((mixed * volume) / 100);
        }
        for (ClientSlot& slot : clients) {
          slot.has_ready_period = false;
        }
      }

      if (!WritePcm(output, reinterpret_cast<const uint8_t*>(out_samples.data()),
                    kPeriodBytes)) {
        if (output != nullptr) {
          pcm_close(output);
          output = nullptr;
        }
      } else {
        last_audio_ms = MonotonicMs();
      }
      continue;
    }

    // power down TDM clock & 4x CS35L41 amps once idle timeout elapsess
    now_ms = MonotonicMs();
    if (output != nullptr && (now_ms - last_audio_ms) >= kIdleTimeoutMs) {
      pcm_close(output);
      output = nullptr;
    }
  }

  for (ClientSlot& slot : clients) {
    CloseSlot(slot);
  }
  if (output != nullptr) {
    pcm_close(output);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (getuid() != 0 || argc != 2) {
    fprintf(stderr,
            "Usage (root recovery only): aera-audio-bridge "
            "--prepare|--sync-rotation|--browser-audio\n");
    return 64;
  }

  if (strcmp(argv[1], "--prepare") == 0) {
    return PrepareMixer() ? 0 : 69;
  }
  if (strcmp(argv[1], "--sync-rotation") == 0) {
    ApplySpeakerRotation(ReadRotationDegrees());
    return 0;
  }
  if (strcmp(argv[1], "--browser-audio") != 0) {
    return 64;
  }

  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, Stop);
  signal(SIGINT, Stop);
  prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);

  const int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (server < 0) return 70;

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path + 1, kSocketName, sizeof(kSocketName) - 1);
  const socklen_t address_size = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + 1 + sizeof(kSocketName) - 1);

  if (bind(server, reinterpret_cast<const sockaddr*>(&address), address_size) != 0) {
    if (errno == EADDRINUSE) {
      fprintf(stderr, "AERA audio: bridge socket already active\n");
    }
    close(server);
    return 70;
  }
  if (listen(server, static_cast<int>(kMaxClients)) != 0) {
    close(server);
    return 70;
  }

  if (!StartBackend()) {
    fprintf(stderr, "AERA audio: Nabu TinyALSA backend failed to start\n");
    close(server);
    return 69;
  }

  fprintf(stderr, "AERA audio: Nabu quad-speaker PCM bridge ready\n");
  RunBridgeServer(server);
  close(server);
  return 0;
}

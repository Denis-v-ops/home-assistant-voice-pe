# Home Assistant Voice: Preview Edition

This is the ESPHome source code of the [Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/).

This fork replaces the Home Assistant Assist audio pipeline with the NOVA
Realtime device protocol. While unmuted, 16 kHz mono PCM streams continuously
over the trusted home LAN to the Portainer gateway in the sibling
`gladosAgent` repository, where the fixed bilingual **Hey Nova** model runs.
Idle audio is not stored or sent to OpenAI; only audio after an accepted wake
handoff enters a realtime session.

This repository is designed to be loaded as a remote ESPHome package. Its
external components are cloned from this repository and its embedded sounds are
downloaded from GitHub at compile time, so no `/config/esphome/components` or
`/config/sounds` directories are required.

Keep the gateway URL and device ID in the ESPHome dashboard's `secrets.yaml`,
then map them into the package from the device YAML:

```yaml
substitutions:
  nova_gateway_url: !secret nova_gateway_url
  nova_device_id: !secret nova_device_id
  nova_repo_ref: nova-v0.3.0
  nova_components_refresh: 1d

packages:
  nova_voice_pe:
    url: https://github.com/Denis-v-ops/home-assistant-voice-PE
    file: home-assistant-voice.yaml
    ref: nova-v0.3.0

```

The package now loads `voice_kit` and `nova_realtime` itself. `nova-v0.3.0` is
the immutable coordinated-release tag; publish it from the reviewed firmware
commit before using this production wrapper. Pre-release CI overrides the
component source and sound path with checked-out local files. Development
packages force-refresh their component checkout so a newer YAML file cannot be
compiled with a stale component implementing an older device protocol.

The gateway URL ends in `/v2/device` and is a direct `ws://` LAN endpoint; this design assumes the
gateway port is not forwarded or otherwise exposed outside the trusted home
network.

### Boot-loop recovery

If a development build enters ESPHome safe mode, its OTA port remains
available. Add this substitution to the installing YAML, clean the build files,
and install wirelessly:

```yaml
substitutions:
  nova_transport_enabled: "false"
```

The recovery build skips all NOVA transport allocation, task creation, and
gateway networking while retaining Wi-Fi, API, logs, and OTA. After collecting
the reset trace and applying a fix, remove the override and reinstall.

Normal builds wait ten seconds before their first gateway connection so API
logs can attach before transport startup. For a longer diagnostic window,
override `nova_connect_delay_ms` with a value up to `60000`.

The center button or **Hey Nova** starts NOVA. The center button rejects a
pending wake or interrupts an active session. There is no microWakeWord, local
wake/stop model, phrase catalog, or sensitivity setting, and the device
deliberately has no Assist or timer fallback. If the gateway is unavailable,
voice wake is unavailable; the center button remains usable once the gateway
connection returns.

During an active NOVA session the device streams XMOS channel 0 with the
AEC/IC/NS/AGC pipeline even while assistant audio is playing. This lets the
gateway's Realtime VAD interrupt playback and recognize conservative standalone
German/English conversation-stop phrases without a device-local stop model.

See [the documentation](https://voice-pe.home-assistant.io/) for set up and troubleshooting.

If you need to re-install the firmware, [use this installer](https://esphome.github.io/home-assistant-voice-pe/).

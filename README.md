# Home Assistant Voice: Preview Edition

This is the ESPHome source code of the [Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/).

This fork replaces the Home Assistant Assist audio pipeline with the NOVA
Realtime device protocol. Wake-word/VAD processing remains local; an active
session streams mono PCM over the trusted home LAN to the Portainer gateway in
the sibling `gladosAgent` repository.

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

packages:
  nova_voice_pe:
    url: https://github.com/Denis-v-ops/home-assistant-voice-PE
    file: home-assistant-voice.yaml
    ref: dev
    refresh: 5min
```

The gateway URL is a direct `ws://` LAN endpoint; this design assumes the
gateway port is not forwarded or otherwise exposed outside the trusted home
network.

The center button or a configured wake word starts NOVA; another wake-word/stop
word or center-button press interrupts it. The device deliberately has no
Assist or timer fallback.

See [the documentation](https://voice-pe.home-assistant.io/) for set up and troubleshooting.

If you need to re-install the firmware, [use this installer](https://esphome.github.io/home-assistant-voice-pe/).

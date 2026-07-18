# Home Assistant Voice: Preview Edition

This is the ESPHome source code of the [Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/).

This fork replaces the Home Assistant Assist audio pipeline with the NOVA
Realtime device protocol. Wake-word/VAD processing remains local; an active
session streams mono PCM over the trusted home LAN to the Portainer gateway in
the sibling `gladosAgent` repository.

Before validating or installing `home-assistant-voice.yaml`, copy the gateway
URL and device ID from `secrets.yaml.example` into the ESPHome dashboard's
`secrets.yaml`. The gateway URL is a direct `ws://` LAN endpoint; this design
assumes the gateway port is not forwarded or otherwise exposed outside the
trusted home network.

The center button or a configured wake word starts NOVA; another wake-word/stop
word or center-button press interrupts it. The device deliberately has no
Assist or timer fallback.

See [the documentation](https://voice-pe.home-assistant.io/) for set up and troubleshooting.

If you need to re-install the firmware, [use this installer](https://esphome.github.io/home-assistant-voice-pe/).

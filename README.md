# Titan Twin Lite

Free, open-source MQTT data binding for Unreal Engine digital twins.

Stream live MQTT data into Unreal: drive Blueprint variables, UMG text and Anim Instance variables from real or simulated devices — no Blueprint polling code.

> **Need OPC UA, HTTP, alarms, commands or history replay?** Get the full version, Titan Twin Pro: https://huahua1.itch.io/titan-twin

## Features
- Native MQTT 3.1.1 client (TLS, QoS 0/1/2, auto-reconnect), networking on a background thread
- JSON / scalar payload parsing, topic → entity mapping
- `TwinEntityComponent` + `TwinPropertyBindingComponent`: bind incoming values to Blueprint properties, UMG widgets and Anim Instances
- UE 5.5, Win64, full C++ source

## Quick start
Topics shaped like `factory/<entity>/<property>` map straight onto actors — no per-topic setup.

1. Copy `TitanTwinLite` into your project's `Plugins/` folder (C++ project, UE 5.5) and open the project.
2. Run an MQTT broker, e.g. [Mosquitto](https://mosquitto.org/) on `127.0.0.1:1883`.
3. Add to `Config/DefaultGame.ini` (or set the same fields in **Project Settings → Game → Titan Twin MQTT / Titan Twin Data Sources**):
   ```ini
   [/Script/TitanTwinMqtt.TwinMqttSettings]
   Host=127.0.0.1
   Port=1883
   bAutoConnect=True
   +Subscriptions=(bEnabled=True,TopicFilter="factory/#",PayloadFormat=Scalar)

   [/Script/TitanTwinDataSource.TwinDataSourceSettings]
   bEnableRuntimeRuleMapping=True
   +AutomaticMappingRules=(bEnabled=True,AddressTemplate="factory/{entity}/{property}",bCaseSensitive=True)
   ```
4. On an actor add **TwinEntityComponent** (Entity Id `robot001`) and **TwinPropertyBindingComponent** with one binding:
   Source Property Name `temperature`, Target Scope `Owner Actor`, Target Variable Name = a float variable on the actor, e.g. `Temperature`.
5. Press Play and publish:
   `mosquitto_pub -h 127.0.0.1 -t factory/robot001/temperature -m 52.3`
   The actor's `Temperature` becomes 52.3.

## Upgrading to Pro
Pro uses the same module names, so remove `TitanTwinLite` before installing Titan Twin Pro. Do not install both in one project.

Full guide: https://qenvaroworks.com/guide.html

## Lite vs Pro

| | Lite (free) | Pro (Fab) |
|---|:-:|:-:|
| MQTT | ✓ | ✓ |
| Property / UMG / Anim binding | ✓ | ✓ |
| OPC UA | | ✓ |
| HTTP polling | | ✓ |
| Alarms (trigger / ack / resolve, hysteresis, debounce) | | ✓ |
| Commands back to devices | | ✓ |
| History & replay | | ✓ |
| Pixel Streaming bridge | | ✓ |
| Support | Issues | Email |

Pro: Titan Twin on Fab — https://qenvaroworks.com/

## License
MIT

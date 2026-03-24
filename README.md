# RemoteSensors

Configured values:
- STA SSID: `NETLIFE-RSNCasa`
- STA password: `csttpstt`
- AP SSID: `APSHRIMP`
- AP password: `csttpstt`

Build and flash:

```bash
source $IDF_PATH/export.sh
idf.py build
idf.py -p /dev/tty.SLAB_USBtoUART flash monitor
```

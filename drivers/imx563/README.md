# IMX563 OIS camera stack (MIPI CSI-2)

Out-of-tree Linux drivers for the Waveshare IMX563 12MP OIS module on a
Raspberry Pi 5 / CM5: sensor (`imx563.c`), autofocus VCM (`dw9800v.c`), and
OIS controller (`ak7323_ois.c`), plus libcamera glue and bring-up tools.

**Status: framework.** Everything builds. The sensor's register tables and
the OIS register map are placeholders, so it won't stream yet. Full plan,
hardware requirements and how to get the missing values:
[`docs/imx563-ois-bringup.md`](../../docs/imx563-ois-bringup.md).

```bash
make                       # against /lib/modules/$(uname -r)/build
sudo make install
grep -rn "TODO(regs)\|TODO(verify)" .
```

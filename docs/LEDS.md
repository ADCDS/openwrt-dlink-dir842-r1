# The DIR-842 panel LEDs — what drives each one, and why ours were wrong

**Status: the five ethernet LEDs are FIXED.** Power needs nothing. WPS exists but is
unwired. The two WiFi LEDs are not done and are a driver-level job, not a GPIO one.

The panel has nine LEDs, left to right:

| # | LED | driven by | status |
|---|---|---|---|
| 1 | Power | nothing — hardwired | ✅ always on, no software involved |
| 2 | Internet | RTL8367S LED group, **port 4** | ✅ fixed |
| 3 | LAN1 | RTL8367S LED group, **port 3** | ✅ fixed |
| 4 | LAN2 | RTL8367S LED group, **port 2** | ✅ fixed |
| 5 | LAN3 | RTL8367S LED group, **port 1** | ✅ fixed |
| 6 | LAN4 | RTL8367S LED group, **port 0** | ✅ fixed |
| 7 | 2.4 GHz | on-SoC WMAC `LEDCFG` register | ✗ not implemented |
| 8 | 5 GHz | RTL8822BE `LEDCFG` register | ✗ not implemented |
| 9 | WPS | SoC GPIO 22 (`dir842:green:wps`) | exists, unwired (`brightness 0`) |

★ **The jack mapping is reversed**: LAN1 = port 3 … LAN4 = port 0. Internet = port 4.
Derived on hardware (§3), not assumed.

---

## 1. There is only ONE GPIO LED on this board, and we already had it

Decoded from the stock `lib/modules/gpiom.ko`, which is **not stripped**. `dev_leds` lives
at `.rodata+0x50`, is 80 bytes, and `dev_leds_num` (`.data+0xa0`) is **4** — so stride 20:

| entry | name | pin | active_low | bank | |
|---|---|---|---|---|---|
| 0 | `wps` | 22 | 1 | **0** | real GPIO → vendor 22 |
| 1 | `fw_update` | 0 | 1 | `0xff` | sentinel, not a pin |
| 2 | `busy` | 0 | 1 | `0xfd` | sentinel |
| 3 | `fake_all` | 0 | 0 | `0xfc` | sentinel |

The method self-validates: run the same decode on `dev_buttons` (`.rodata+0xa0`, 39 bytes =
3 × 13, fields `char name[10]; u8 pin; u8 active_low; u8 bank`) and it yields reset=54,
wps=56, wifi=36 — exactly the values already in the DTS.

★ So **stock has exactly one GPIO LED, WPS on GPIO 22, and our DTS already matches it.**
Everything else on the panel is driven by hardware blocks, not pins. Do not go looking for
more `gpio-leds` — there are none.

## 2. What stock actually does: `eth_leds_ctrl`

`gpiom.ko` imports `eth_leds_ctrl`, which is not in any `.ko` — it is built into the stock
kernel. Decompress `mtd3-kernel.bin` (LZMA payload at file offset `0x3818`), resolve via
`__ksymtab` at link base `0x80000000`, and it lands at **`0x801b90e8`**:

```
eth_leds_ctrl(mask):
    for port in 0..5:
        a2 = (mask & (1 << port)) ? 3 : 2
        led_set(port, group=0, mode=a2)

led_set(port<8, group<3, mode<4)  ->  0x801cc7a0:
    v0 = port << 1
    a0 = (group << 1) + 0x1B08        ; 0x1B08 / 0x1B0A / 0x1B0C
    a1 = 3 << (port*2)                ; 2-bit field mask
    reg_field_write(a0, a1, mode)
```

So stock **force-drives every jack LED from software**, writing mode 3 for a lit port and
mode 2 for a dark one on every link change. That is why stock's LEDs look right: it
overrides the hardware constantly.

There is a second, unrelated mechanism at `0x80152050` — a read-modify-write of bits
[11:8] of `0xB8003504` (the SoC GPIO block's `+0x04` register):

```
lui v1,0xb800 ; li a1,0xfffff0ff ; lw v0,0x3504(v1)
sll a0,a0,0x8 ; and v0,v0,a1 ; or v0,v0,a0 ; sw v0,0x3504(v1)
```

This is real code and confirms the claim that used to sit in `board.d/01_leds`. We do not
need it for the panel LEDs, and it is unreachable from userspace anyway (`CONFIG_DEVMEM`
is off — there is no `/dev/mem`).

## 3. The register that matters, and its modes — measured

Group registers `0x1B08` / `0x1B0A` / `0x1B0C`, **2 bits per port** at bit `port*2`.
All four modes verified on hardware by setting a different one per port and reading the
panel:

| mode | effect | how it was shown |
|---|---|---|
| **0** | **hardware indication (link + activity)** | cabled jack blinks with traffic; dark jacks stay dark |
| 1 | force blink | port1 → LAN3 blinking with no cable |
| 2 | force off | port2 → dark *despite link up* |
| 3 | force on | port3 → LAN1 lit with no cable; `0x1B08=0xffff` lit all five |

The port→jack mapping falls straight out of that same reading: with
`0x1B08 = 0x00e4` (port0=0, port1=1, port2=2, port3=3, port4=0) the panel showed **LAN1 on,
LAN3 blinking, everything else off** — so LAN1=port3, LAN3=port1, and by elimination
LAN2=port2, LAN4=port0, Internet=port4.

## 4. ★ Why ours were wrong

The bootloader leaves group 0 at **`0x1B08 = 0x0afe`**:

| port | jack | mode | effect |
|---|---|---|---|
| 0 | LAN4 | 2 | forced OFF |
| 1 | LAN3 | 3 | forced ON |
| 2 | LAN2 | 3 | forced ON |
| 3 | LAN1 | 3 | forced ON |
| 4 | Internet | 2 | forced OFF |

Three LAN LEDs lit regardless of link, Internet and LAN4 dead. **`rtl8367b.c` contained no
LED code at all**, so whatever the loader left simply persisted for the life of the boot.

## 5. The fix

`rtl8367b_setup()` now clears the force-mode fields for all three groups, handing the LEDs
back to the switch:

```c
for (i = 0; i < RTL8367B_NUM_LED_GROUPS; i++)
	REG_WR(smi, RTL8367B_LED_FORCE_MODE_REG(i), 0);
```

This is smaller *and* better than replicating stock: mode 0 is hardware indication, so
there is no software in the path, nothing to poll, and nothing to get out of sync on a link
change. A board that genuinely wants a forced LED can set its own field afterwards.

## 6. Debugging LEDs live, without a rebuild

The switch driver exposes `reg`/`val` under debugfs — this is how everything above was
measured:

```sh
rd(){ echo $1 > /sys/kernel/debug/rtl8367b/reg; cat /sys/kernel/debug/rtl8367b/val; }
wr(){ echo $1 > /sys/kernel/debug/rtl8367b/reg; echo $2 > /sys/kernel/debug/rtl8367b/val; }

rd 0x1b08              # per-port LED force modes, group 0
wr 0x1b08 0xffff       # force every jack LED on
wr 0x1b08 0x0000       # hand them back to hardware  <- the shipped state
```

★ Values must carry the `0x` prefix; `echo 2040` is silently ignored, `echo 0x2040` works.
Sanity-check the interface with `rd 0x1300` → `0x6367` and `rd 0x1301` → `0x0020`
(RTL8367S). These registers are LED-only — writing them cannot affect the datapath.

## 7. What is left

- **2.4 GHz / 5 GHz.** Not GPIOs. Stock drives them through the WiFi MAC's own `LEDCFG`
  register: `control_wireless_led()` → `set_sw_LED0/1/2()` → `RTL_W32(LEDCFG, ...)` in
  `8192cd_led.c` (which ships in this tree), and the 5 GHz side would need the equivalent
  in rtw88. Neither is wired up.
  ★ Do **not** be misled by the `0xb8003500`/`0x3508`/`0x350c` GPIO writes in
  `8192cd_host.c` — that is `GPIO6_PCIE_Device_PERST()`, guarded by
  `#if defined(CONFIG_RTL_8196CS) || defined(CONFIG_RTL_8197B)`, so it is PCIe reset on
  other SoCs and is not even compiled for the 8197F.
- **WPS.** The LED device exists (`dir842:green:wps`, GPIO 22) and sits at `brightness 0`.
  Wiring it to a trigger in `board.d/01_leds` is a one-liner whenever it is wanted.
- **Power.** Nothing to do; it has no GPIO in stock either (its `libdhal.so` entry points
  are stubs) and it is lit.


## ★ WiFi panel LEDs — FIXED (issue #3)

Both WiFi LEDs are driven by the respective WiFi MAC, not by SoC GPIOs (see the `gpiom.ko`
decode above — there is no WiFi LED GPIO in stock). Both are now lit. Verified on the panel
2026-09-02.

### 2.4 GHz — vendor `rtl8192cd`: a MIB knob, no driver change

The driver has full LED plumbing (`8192cd_led.c`); it was dark only because the MIB
`led_type` defaulted to **0 = `LEDTYPE_HW_TX_RX`**, a hardware-indication mode this board does
not wire. Setting a *software* type makes `enable_sw_LED()` configure the LED on open (the SoC-WMAC
branch sets the LED's enable bit in `LEDCFG` alongside its software value -- the exact
`LEDxEN` written depends on the type; the `LED2EN|LED2SV` line quoted in an earlier draft is
the 8188E branch) and the driver's LED task then blinks it with link/traffic, as stock does.

★ **Diagnosis that found it, worth keeping:** `echo 1 > /proc/wlan0/led` (the proc entry is
*write-only* — reading it gives `I/O error`) calls `control_wireless_led(priv, 1)`, which
forces the three LED **software values** on and produced only a **faint glow**. The proper path
also sets the **enable** bit; the difference between "faint" and "strong" is `LED2EN`. So: a
faint LED means SV without EN.

Shipped in `lib/netifd/wireless/rtl8192cd.sh`: `${phy_ifname}_led_type=${led_type:-11}` in the
driver's `.dat` (11 = `LEDTYPE_SW_LED2_GPIO8_LINKTXRX`, the vendor default in one of its two
`set_mib` tables, `8192cd_ioctl.c:1429`). The `RTLWIFINIC_GPIO_CONTROL` bit-bang path is
`#undef`'d in `8192cd_cfg.h`, so this is the `LEDCFG` register path. Override with uci
`option led_type N` on the radio if a different blink policy is wanted.

### 5 GHz — rtw88 patch `06-rtw88-8822b-panel-led.patch`

rtw88 5.8-rc2 has no LED support. The vendor driver's `set_sw_LED0()` for `VERSION_8822B`
drives the card's **GPIO8: register `0x60` bit 8, active-low** (set = off, clear = on),
touching no other bit — rtw88's own writes to `0x62` are SDIO-only, so on PCIe nothing else
configures that pin. The patch mirrors the vendor: once at start it performs the vendor's
`enable_sw_LED()` 8822B setup (`LEDCFG &= ~LED2EN`, then `0x60 |= BIT(16)|BIT(24)` -- GPIO8
output-enable and mode; a first cut omitted these and the review caught that the OE byte read
back as 0), then LED on at the end of `rtw_core_start()`, off at the top of
`rtw_core_stop()`, i.e. **LED follows the radio**. To disable: add the line
`rtw88_core led=0` to `/etc/modules.d/rtw88` (kmodloader reads options only from there; a
kernel-cmdline `rtw88_core.led=0` is honoured by nothing on this image), or at runtime
`echo 0 > /sys/module/rtw88_core/parameters/led` followed by `wifi down; wifi up` -- the
disabled path writes the OFF value once so a lit LED does not stay lit. dmesg on every
bring-up:

```
rtw_8822be 0000:01:00.0: panel LED on (reg 0x60=0x0101000b)
```

(You will see `on` → `off` → `on` at boot: mac80211 restarts the radio once during setup.)

### Still unwired

* **WPS** (`dir842:green:wps`, GPIO 22) — the LED device exists at brightness 0 and is not
  bound to any trigger. One line in `board.d/01_leds` if anyone wants it.

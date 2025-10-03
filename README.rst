===========
JKDBGVM README
===========

Reduction summary
-----------------

* 전체 문서 트리(`docs/`)를 삭제했습니다. Sphinx 템플릿과 사용자/시스템/개발자
  매뉴얼, 개별 기능 설명서 등이 모두 제거되어 빌드 산출물에는 문서가 포함되지
  않습니다. `./configure`는 문서 부재를 자동으로 감지하여 `docs: disabled`로
  동작합니다.
* 상위 빌드 스크립트(`meson.build`)는 삭제된 서브트리의 존재 여부를 검사하도록
  정리했습니다. `contrib/plugins`, `tests/qtest`, `docs` 등이 사라져도 빌드가
  중단되지 않으며, `sphinx_build`와 `build_docs`를 `false`로 고정해 문서 빌드를
  완전히 건너뜁니다.
* 레거시 오디오 서브시스템을 통째로 제거했습니다. 최상위 `audio/`, `hw/audio/`,
  `include/hw/audio/` 트리와 USB/virtio 오디오 경로(`hw/usb/dev-audio.c`,
  `hw/virtio/vhost-user-snd*.c`)가 모두 삭제되었고, `qemu-options.hx`,
  `qapi/*`, `hmp-commands*.hx`, `vl.c`에서 관련 옵션과 명령이 사라졌습니다.
* VMApple 구성 레지스터에서 Wi-Fi/Bluetooth 슬롯을 제거했습니다
  (`hw/vmapple/cfg.c`). `mac_wifi0`/`mac_bt0` MAC 주소 필드 대신 동일 크기의
  예약 영역을 채워 게스트가 더 이상 무선 MAC을 받지 못합니다. 게스트에서
  `0x00400000` 구성 영역을 덤프해 `0x50`·`0x58`가 0x00으로 채워졌는지 확인할 수
  있습니다.
* iPXE 번들에서 무선 네트워크 스택을 제거했습니다. `roms/ipxe/src/config/general.h`
  의 802.11 매크로를 비활성화하고, `roms/ipxe/src/net/80211/`,
  `roms/ipxe/src/drivers/net/{ath*,rtl818x,prism2*}`, `roms/ipxe/src/include/ipxe/*80211*.h`,
  `roms/ipxe/src/usr/iwmgmt.c` 등을 통째로 삭제해 무선 관련 바이너리가 생성되지
  않습니다.
* x86_64 기본 구성에서 Q35 한 종만 남기고 나머지 PC 머신들을 비활성화했습니다
  (`configs/devices/x86_64-softmmu/default.mak`). `CONFIG_I440FX`, `CONFIG_MICROVM`,
  `CONFIG_PIIX` 등 레거시 머신이 모두 `n`으로 고정되어 I440FX/ISA 변형이 빌드되지
  않습니다.
* 같은 파일에서 USB 스택을 전부 비활성화했습니다. `CONFIG_USB*`와 연관 옵션을
  모두 `n`으로 고정했기 때문에 libusb, usb-redir, U2F, 스마트카드 등 USB 경로가
  빌드되지 않습니다. `build/x86_64-softmmu-config-devices.mak`에도 USB 관련 항목이
  존재하지 않습니다.
* Rust 지원을 완전히 제거했습니다. `meson_options.txt`, `configure`,
  `scripts/meson-buildoptions.sh`에서 `--enable-rust`와 `CONFIG_HAVE_RUST`를 제거하고,
  `rust/`, `scripts/rust/`, `scripts/get-wraps-from-cargo-registry.py`, `clippy.toml` 등
  관련 트리를 삭제했습니다.

Rebuild recipe
--------------

::

  ./configure --target-list=x86_64-softmmu \
              --disable-docs \
              --disable-libusb --disable-usb-redir \
              --disable-u2f --disable-smartcard \
              --enable-slirp \
              --enable-gtk --disable-vnc --disable-sdl --disable-spice
  ninja -C build

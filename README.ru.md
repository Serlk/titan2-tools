<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# titan2-tools: mainline Linux на Unihertz Titan 2 (alpha1)

[English version](README.md)

Здесь лежат инструменты, которые собирают и проверяют загрузочные образы для первого
mainline-ядра Linux (7.2) на **Unihertz Titan 2** (MediaTek MT6878 / Dimensity 7300). Образы
загружает **стоковый загрузчик LK, его мы не меняем**. Само ядро находится в
[titan2-linux](../titan2-linux) (ветка `titan2`). Документация (требования LK, попытки загрузки,
описание железа) — в [titan2-docs](../titan2-docs). Образы alpha1 приложены к GitHub-релизу
`alpha1` этого репозитория.

> **Это альфа для разработчиков.** Система загружается до root-консоли по USB. Графики, телефонии
> и Wi-Fi нет, управление батареей не проверено. Ставьте, только если умеете восстановить телефон
> через fastboot. Всё делаете на свой риск.

## Что работает в alpha1 (4-я загрузка на железе)

| | |
|---|---|
| Экран | Да: фреймбуфер, который оставляет LK (simpledrm, 1440x1440), и запуск кадров `mtk-cmdfb-kick` для DSI-панели в командном режиме. Текстовая консоль выводится на экран. |
| Клавиатура | Да: матрица AW9523B (`aw9523-keypad`) |
| Тачскрин, тачпад | Да: Synaptics RMI4 по I2C |
| Громкость, Func2 | Да: gpio-keys |
| Кнопка питания, Func1 (home) | **Исправлено, на железе не проверено**: прерывания PMIC через SPMI, исправлена таблица EINT |
| Накопитель | UFS определяется. initramfs **принудительно переводит все блочные устройства в read-only**. |
| USB | Да: гаджет NCM (сеть 172.16.42.1) и ACM (консоль `ttyGS0`), SSH (dropbear) |
| Регуляторы | Да: MT6363/MT6369 (только линии, которые в стоке включены всегда) |
| Зарядка | Экспериментальный драйвер (MT6375 + датчик SH366100), ограничение 4,35 В. **Не проверено.** |
| Чтение `/sys/kernel/debug/clk/clk_summary` | Раньше вешало шину. Контроллеры тактов GPU/SCP теперь отключены; **не проверено** |
| Не поддерживается | GPU, модем/звонки, Wi-Fi/Bluetooth, звук, камеры, датчики, сон, второй (задний) экран |

## Что нужно

- Unihertz Titan 2 на стоковой прошивке **V01.00.14**, **разблокированный загрузчик**
  (`fastboot flashing unlock` стирает данные).
- `vbmeta_a` с отключённой проверкой. Если вы уже ставили GSI, это сделано. Иначе выполните
  `fastboot --disable-verity --disable-verification flash vbmeta_a vbmeta.img`, взяв стоковый
  `vbmeta.img` **вашей** версии прошивки.
- **Только слот A.** Проверьте командой `fastboot getvar current-slot`. **Не** выполняйте
  `fastboot set_active b`: в слоте B старые загрузчики.
- **Бэкап** `boot_a`, `init_boot_a`, `vendor_boot_a`, `dtbo_a` (и `vbmeta_a`) **вашего** телефона,
  снятый **до** прошивки, с файлом `sha256sum *.img > SHA256SUMS`. Образы можно взять из
  официального пакета той же версии прошивки или снять на телефоне с root
  (`dd if=/dev/block/by-name/boot_a of=/sdcard/boot_a.img` и так далее).
- ПК с Linux, `fastboot` и `ssh`. Для сборки: Docker, git, AOSP clang r574158.

## Установка образов alpha1

В релизе два варианта, ядро и initramfs у них одинаковые:

- `usb-minimal`: devicetree без SPMI/PMIC, клавиатуры, тачскрина и тачпада (экран, UFS, USB,
  громкость). Начните с него.
- `usb`: всё перечисленное выше.

```
sha256sum -c SHA256SUMS
adb reboot bootloader            # или: громкость вверх + питание -> «Fastboot Mode»
fastboot getvar current-slot     # должно быть: a
cd usb-minimal                   # или: cd usb
fastboot flash boot_a        boot.img
fastboot flash init_boot_a   init_boot.img
fastboot flash vendor_boot_a vendor_boot.img
fastboot flash dtbo_a        dtbo.img
fastboot reboot
```

Когда ядро запустится, ПК увидит USB-сетевой адаптер и последовательный порт. Адрес телефона —
172.16.42.1, ПК получает адрес по DHCP:

```
ssh root@172.16.42.1             # в образах релиза у root ПУСТОЙ пароль
t2-report > t2-report.txt        # dmesg, прерывания, регуляторы, такты, I2C, ввод
t2-noreboot                      # отменить страховочную перезагрузку
```

**Страховочная перезагрузка:** если за 10 минут не было входа по SSH или по последовательной
консоли, телефон перезагрузится сам (`t2-noreboot` это отменяет). Android не загрузится, пока вы
не вернёте четыре раздела (см. ниже).

**Безопасность:** initramfs релиза пускает `root` **без пароля**, но только по USB-кабелю (другой
сети нет). Чтобы входить только по ключу, соберите свой initramfs с открытым ключом:
`T2_AUTHORIZED_KEYS=~/.ssh/id_ed25519.pub`.

**Личные данные:** в выводе `t2-report` и `dmesg` есть серийный номер устройства, идентификатор
UFS и MAC-адреса. Удалите их, прежде чем публиковать журналы.

## Возврат на Android

```
scripts/restore-stock-boot.sh /путь/к/вашему/бэкапу     # ждёт fastboot, прошивает, перезагружает
```

Или вручную: `fastboot flash boot_a boot_a.img`, затем так же `init_boot_a`, `vendor_boot_a` и
`dtbo_a`, затем `fastboot reboot`.

**Как попасть в fastboot из цикла перезагрузок:** не отключая кабель, держите **громкость вверх +
питание**, пока не появится меню «Select Boot Mode». Громкость вверх перемещает выбор, громкость
вниз подтверждает; выберите **Fastboot Mode**. Одна громкость вверх запускает recovery, а он
грузит то же ядро из `boot_a`. Долгое нажатие питания перезагружает телефон, а не выключает.

**Никогда** не прошивайте и не стирайте `preloader`, `lk`, `tee`, `seccfg`, `nvram`, `nvdata`,
`nvcfg`, `proinfo`, `protect1/2`, `md*` и не блокируйте загрузчик. Не выполняйте `fastboot -w`.
Эти образы ничего не пишут во внутреннюю память: все блочные устройства только для чтения.

## Сборка

```
git clone <titan2-tools> && cd titan2-tools
git clone -b titan2 <titan2-linux> linux        # дерево ядра (или kernel-source-alpha1.tar.gz + apply.sh)
# AOSP clang r574158 (clang 21) в toolchains/clang-r574158:
#   из https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86 (каталог clang-r574158)
# AOSP mkbootimg (Apache-2.0, не входит в репозиторий) в tools/mkbootimg:
git clone https://android.googlesource.com/platform/system/tools/mkbootimg tools/mkbootimg
docker build -t titan2-kbuild docker/
make -C tools CC=$PWD/toolchains/clang-r574158/bin/clang     # mini-init, devmem
scripts/build-mainline.sh                                   # -> out/mainline-build
USB=1 VARIANT=minimal scripts/make-mainline-images.sh       # -> out/mainline-images-usb-minimal
USB=1 scripts/make-mainline-images.sh                       # -> out/mainline-images-usb
docker run --rm -u $(id -u):$(id -g) -v "$PWD":/work titan2-kbuild \
    python3 /work/scripts/lk-fdt-check.py --vendor-boot /work/out/mainline-images-usb/vendor_boot.img
```

initramfs (`tools/initramfs/build.sh`) скачивает пакеты Alpine v3.24 aarch64 (busybox, dropbear,
i2c-tools, evtest, musl и другие) и проверяет их подписи. `tools/initramfs/qemu/run.sh` проверяет
его в qemu. Ничто в этом репозитории не прошивает телефон.

| путь | что делает |
|---|---|
| `scripts/build-mainline.sh` | сборка ядра (`Image.gz`) и DTB в Docker |
| `scripts/make-mainline-images.sh` | образы boot/vendor_boot/init_boot/dtbo v4 в том виде, в каком их ждёт стоковый LK |
| `scripts/lk-fdt-check.py` | офлайн-проверка DTB или vendor_boot на известные требования LK |
| `scripts/restore-stock-boot.sh` | прошивает обратно ваш бэкап слота A |
| `tools/initramfs/` | initramfs «USB-консоль»: проверенные пакеты Alpine и наши скрипты |
| `tools/mini-init/`, `tools/devmem/` | статические утилиты aarch64 без libc |
| `configs/mainline-titan2.config` | фрагмент конфигурации ядра поверх arm64 `defconfig` |
| `docker/Dockerfile` | контейнер сборки `titan2-kbuild` |

## Как делается проект и с кем он связан

- Проект разрабатывается с помощью ИИ-ассистентов (Anthropic Claude). В коммитах стоит строка
  `Co-Authored-By: Claude`; каждое изменение проверяет и испытывает на устройстве человек —
  мейнтейнер (serlk), он же за него отвечает.
- Проект **не связан с postmarketOS** и MT6878-Mainline. Мы опираемся на их GPL-работу над ядром
  (спасибо!), но postmarketOS не принимает вклад, сделанный с помощью ИИ, поэтому отсюда ничего
  не отправляется в postmarketOS, и это не порт postmarketOS.

## Источники

Сведения, на которых построены инструменты (адреса загрузки, требования стокового
загрузчика к образам и дереву устройств, значения регистров), получены с самого
телефона: журналы загрузки, чтение на устройстве, заводское дерево устройств,
собственные модули ядра телефона, объявленные под GPL, и анализ поведения
стокового загрузчика (его код не воспроизводится); для сравнения — публичные
исходники MediaTek под GPL и upstream Linux. Проприетарного кода, прошивок и бинарных
файлов здесь нет.

## Лицензия

Copyright (c) 2026 serlk. GPL-2.0-or-later, см. [LICENSE](LICENSE). Лицензия каждого файла
указана в строке `SPDX-License-Identifier`. `tools/initramfs/keys/*.pub` — открытый ключ подписи
Alpine Linux (из пакета Alpine `alpine-keys`, MIT). Ядро распространяется под GPL-2.0
(titan2-linux). Сторонние пакеты в initramfs релиза и где взять их исходники — файл
`initramfs-packages.md` в релизе.

## Благодарности

- [MT6878-Mainline](https://github.com/MT6878-Mainline/linux) (Igor Belwon, Arseniy Velikanov и
  другие участники) и [postmarketOS](https://postmarketos.org/): поддержка SoC MT6878, на которой
  построен этот порт, и соглашения USB-консоли.
- MediaTek: GPL-исходники ядра для MT6878, опубликованные через Nothing (CMF Phone 1). Из них
  взяты описания регистров для драйверов PMIC, тактов и доменов питания.
- Alpine Linux, BusyBox и Dropbear.

Проект не связан с Unihertz и MediaTek.

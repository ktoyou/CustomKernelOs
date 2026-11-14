# MyOS — сборка
#
#   make           — собрать ядро (build/os.img), программы и установить их в /bin на disk.img
#   make run       — всё собрать и запустить в QEMU
#   make test      — юнит-тесты файловой системы и кучи (на хосте)
#   make jstest    — тесты JavaScript, DOM и CSS браузера (на хосте)
#   make gfxtest   — пробная картинка сглаженной графики и шрифтов (build/gfxtest.ppm)
#   make clean     — удалить build/ (диск с файлами disk.img НЕ трогается)
#   make cleandisk — удалить disk.img (при следующем запуске ФС создастся заново)
#
#   build/myfs disk.img ls /bin            — посмотреть диск с компьютера
#   build/myfs disk.img put file.txt /f    — положить свой файл на диск
#   build/myfs disk.img cat /etc/display   — вывести файл с диска
#   build/myfs disk.img grow 8192          — расширить ФС до 8 ГБ, старый образ остаётся как disk.img.bak

# На macOS (и вообще не на x86-Linux) системный gcc — это clang под arm64/x86_64-darwin,
# он не умеет собирать 32-битный ELF. Нужен кросс-тулчейн:
#   brew install i686-elf-gcc i686-elf-binutils nasm qemu
# CLI overrides > process environment > .env > defaults.
# Parse .env as data, never as shell or Make code. Each configuration gets its
# own immutable include so concurrent Make invocations cannot overwrite it.
ENV_CONFIG_MK := $(shell python3 tools/env_config.py --make-file)
ifeq ($(strip $(ENV_CONFIG_MK)),)
$(error Cannot load .env configuration; see the diagnostic above)
endif
include $(ENV_CONFIG_MK)

# переносимые (GNU/BSD) размер файла и дополнение файла нулями до размера
fsize   = $$(wc -c < $(1) | tr -d ' ')
padto   = dd if=/dev/null of=$(1) bs=1 seek=$(2) 2>/dev/null

# Код gcc хоста целиком копируется в ОЗУ. -Oz жмёт текст сильнее -O2.
# На i386 кадр через ebp остаётся даже при -Os, поэтому убираем его явно.
# Таблицы раскрутки freestanding-программе не нужны, а лежат в загружаемом файле.
SIZE_FLAGS := -Oz -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables
CFLAGS  := -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector \
           -fno-builtin -nostdlib -nostdinc -mno-sse -mno-mmx -mno-80387 \
           $(SIZE_FLAGS) -Wall -Wextra -std=gnu11
LDFLAGS := -m elf_i386 -T linker.ld -nostdlib \
           $(shell $(LD) --help 2>/dev/null | grep -q no-warn-rwx && echo --no-warn-rwx-segments)

BUILD   := build
HWBUILD := $(BUILD)/hardware
# Новый disk.img, если файла ещё нет. Готовый образ сам не растёт:
# build/myfs disk.img grow 8192  (прежний остаётся как disk.img.bak)
# 4 ГБ — разреженный файл: место на компьютере занимает только записанное.
# Потолок ФС — 8 ГБ (FS_MAX_BLOCKS). Версия 2 хранит файл любого размера в этих пределах.
VIDEO_SRC := $(wildcard drivers/video/*.c)
C_SRC   := $(wildcard kernel/*.c)
OBJS    := $(BUILD)/entry.o $(BUILD)/isr.o $(BUILD)/ap.o $(patsubst kernel/%.c,$(BUILD)/%.o,$(C_SRC)) $(patsubst drivers/video/%.c,$(BUILD)/video/%.o,$(VIDEO_SRC))

all: $(BUILD)/os.img install

$(BUILD)/user/launchbench.o: tests/launchbench.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/launchbench: $(BUILD)/user/launchbench.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

$(BUILD)/user/spawncheck.o: tests/spawncheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/spawncheck: $(BUILD)/user/spawncheck.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

$(BUILD)/user/desktop_launchcheck.o: tests/desktop_launchcheck.c user/desktop.c user/desktop_gpu.inc user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/desktop_launchcheck: $(BUILD)/user/desktop_launchcheck.o $(BUILD)/user/l_img.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(BUILD)/user/l_img.o $(ULIB) -o $@

.PHONY: launchtest
launchtest:
	python3 tests/launch_smoke.py

.PHONY: smptest
smptest:
	python3 tests/smp_smoke.py

$(BUILD)/user/smpcheck.o: tests/smpcheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/smpcheck: $(BUILD)/user/smpcheck.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

$(BUILD)/user/smpbench.o: tests/smpbench.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/smpbench: $(BUILD)/user/smpbench.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

.PHONY: FORCE_BUILD_INFO
FORCE_BUILD_INFO:
$(BUILD)/build_info.h: FORCE_BUILD_INFO VERSION tools/build_info.py | $(BUILD)
	python3 tools/build_info.py $@
$(BUILD)/virtgpu.o: kernel/virtgpu_render.inc kernel/virtgpu_cursor.inc
$(BUILD)/sysinfo.o: $(BUILD)/build_info.h
$(OBJS): user/lib/sysinfo.h user/lib/hwinfo.h
$(BUILD)/user/libc.o $(BUILD)/user/libc.so: user/lib/sysinfo.h user/lib/hwinfo.h
CFLAGS += -I$(BUILD)


# Native clipboard/editor checks, installed only onto a disposable test disk.
$(BUILD)/user/clipboardcheck.o: tests/clipboardcheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/clipboard_editor.o: tests/clipboard_editor.c user/notepad.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/clipboard_terminal.o: tests/clipboard_terminal.c user/terminal.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/clipboardcheck: $(BUILD)/user/clipboardcheck.o $(BUILD)/user/clipboard_editor.o $(BUILD)/user/clipboard_terminal.o $(BUILD)/user/libmyos.a
	$(LD) -m elf_i386 -T user/user.ld -nostdlib -s --gc-sections $^ -o $@

$(BUILD)/user/pr2check.o: tests/pr2check.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/pr2check: $(BUILD)/user/pr2check.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

.PHONY: pr2test
pr2test:
	python3 tests/pr2_smoke.py

.PHONY: clipboardtest
clipboardtest:
	python3 tests/clipboard_smoke.py
	python3 tests/clipboard_gui_smoke.py

$(BUILD) $(BUILD)/user:
	mkdir -p $@

$(BUILD)/video/%.o: drivers/video/%.c drivers/video/*.h kernel/*.h
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/fb.o $(HWBUILD)/fb.o: drivers/video/video.h
$(HWBUILD)/video/%.o: drivers/video/%.c drivers/video/*.h kernel/*.h
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -DMYOS_HARDWARE -c $< -o $@

# ------------------------------------------------------------------ ядро

$(BUILD)/%.o: kernel/%.c kernel/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: kernel/%.asm | $(BUILD)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/kernel.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# Загрузчику нужно знать размер ядра в секторах — считаем и передаём в NASM
$(BUILD)/boot.bin: boot/boot.asm $(BUILD)/kernel.bin
	$(NASM) -f bin -DKERNEL_SECTORS=$$(( ($(call fsize,$(BUILD)/kernel.bin) + 511) / 512 )) $< -o $@

# Образ: [сектор 0: boot.bin][сектора 1..N: kernel.bin], дополнен до 1.44 МБ
$(BUILD)/os.img: $(BUILD)/boot.bin $(BUILD)/kernel.bin
	cat $^ > $@
	$(call padto,$@,1474560)
	@echo "==> $@ готов (ядро: $(call fsize,$(BUILD)/kernel.bin) байт)"

# ------------------------------------------------------------------ программы (ring 3)

# программам FPU можно (ядро сохраняет его регистры при переключении), SSE — нет
UCFLAGS  := -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector \
            -fno-builtin -nostdlib -nostdinc -mno-sse -mno-mmx \
            $(SIZE_FLAGS) -Wall -std=gnu11 -Iuser/include -Iuser/lib -ffunction-sections -fdata-sections
# --gc-sections и библиотека-архив: в программу попадает только то, что она зовёт
ULDFLAGS := -m elf_i386 -T user/user.ld -nostdlib -s --gc-sections
# ldso.c — не команда /bin, а исходник /lib/ld.so (собирается отдельно, своим cc).
PROGRAMS := $(filter-out ldso,$(basename $(notdir $(wildcard user/*.c)))) cc
UBINS    := $(addprefix $(BUILD)/user/,$(PROGRAMS))

ULIB_OBJS := $(BUILD)/user/libc.o $(BUILD)/user/setjmp.o $(BUILD)/user/alloca.o $(BUILD)/user/zlib.o $(BUILD)/user/gfx.o $(BUILD)/user/vec.o $(BUILD)/user/font.o $(BUILD)/user/win.o
ULIB := $(BUILD)/user/libmyos.a

$(ULIB): $(ULIB_OBJS)
	rm -f $@ && $(AR) rcs $@ $^

$(BUILD)/user/libc.o: user/lib/libc.c user/lib/regex.c user/include/regex.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/setjmp.o: user/lib/setjmp.S | $(BUILD)/user
	$(CC) -m32 -c $< -o $@

$(BUILD)/user/alloca.o: user/lib/alloca.S | $(BUILD)/user
	$(CC) -m32 -c $< -o $@

$(BUILD)/user/zlib.o: user/lib/zlib.c user/include/zlib.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/gfx.o: user/lib/gfx.c user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/vec.o: user/lib/vec.c user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/font.o: user/lib/font.c user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/win.o: user/lib/win.c user/lib/win.h user/lib/gfx.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/code.o: user/code_features.h user/code_index.h user/code_workspace.h user/code_chrome.h user/code_signature.h

$(BUILD)/user/%.o: user/%.c user/lib/myos.h user/lib/hwinfo.h user/lib/sysinfo.h user/lib/gfx.h user/lib/win.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

# sh и cc остаются статическими: оболочка и компилятор стартуют даже со сломанной libc.so.
$(BUILD)/user/sh: $(BUILD)/user/sh.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

# Динамические программы: libc.so/libgfx.so, интерпретатор /lib/ld.so.
DYNLDFLAGS := -m elf_i386 -nostdlib -s --gc-sections -dynamic-linker /lib/ld.so \
              --hash-style=sysv --as-needed -Ttext-segment=0x40000000

$(BUILD)/user/crt0.o: user/lib/crt0.asm | $(BUILD)/user
	$(NASM) -f elf32 $< -o $@

$(BUILD)/user/libc.so: user/lib/libc.c user/lib/setjmp.S user/lib/alloca.S user/lib/regex.c user/include/regex.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -fPIC -D__LIBC_SO__ -shared -Wl,-soname,libc.so -Wl,--hash-style=sysv -o $@ user/lib/libc.c user/lib/setjmp.S user/lib/alloca.S

$(BUILD)/user/pic/%.o: user/lib/%.c user/lib/myos.h | $(BUILD)/user/pic
	$(CC) $(UCFLAGS) -fPIC -c $< -o $@

$(BUILD)/user/pic: | $(BUILD)/user
	mkdir -p $@

PIC_GFX := $(patsubst %,$(BUILD)/user/pic/%.o,gfx vec font win)
$(BUILD)/user/pic/gfx.o: user/lib/gfx.h
$(BUILD)/user/pic/vec.o: user/lib/gfx.h
$(BUILD)/user/pic/font.o: user/lib/gfx.h
$(BUILD)/user/pic/win.o: user/lib/win.h user/lib/gfx.h

$(BUILD)/user/libgfx.so: $(PIC_GFX) $(BUILD)/user/libc.so
	$(CC) -m32 -shared -nostdlib -Wl,-soname,libgfx.so -Wl,--hash-style=sysv -o $@ $(PIC_GFX) -L$(BUILD)/user -lc

$(BUILD)/user/pic/zlib.o: user/include/zlib.h
$(BUILD)/user/libz.so: $(BUILD)/user/pic/zlib.o $(BUILD)/user/libc.so
	$(CC) -m32 -shared -nostdlib -Wl,-soname,libz.so -Wl,--hash-style=sysv -o $@ $(BUILD)/user/pic/zlib.o -L$(BUILD)/user -lc

# libm.so — единственная математика: src/math musl (float и double).
MUSL_MATH_SRC := $(wildcard user/lib/musl-math/*.c)
MUSL_PIC := $(patsubst user/lib/musl-math/%.c,$(BUILD)/user/pic/m_%.o,$(MUSL_MATH_SRC))
$(BUILD)/user/pic/m_%.o: user/lib/musl-math/%.c $(wildcard user/lib/musl-math/*.h) user/include/math.h user/include/fenv.h user/include/stdint.h | $(BUILD)/user/pic
	$(CC) $(UCFLAGS) -fexcess-precision=standard -fPIC -Iuser/lib/musl-math -c $< -o $@
$(BUILD)/user/libm.so: $(MUSL_PIC) $(BUILD)/user/libc.so
	$(CC) -m32 -shared -nostdlib -Wl,-soname,libm.so -Wl,--hash-style=sysv -o $@ $(MUSL_PIC) -L$(BUILD)/user -lc

$(BUILD)/user/pic/libdl.o: user/lib/libdl.c user/include/dlfcn.h | $(BUILD)/user/pic
	$(CC) $(UCFLAGS) -fPIC -c $< -o $@
$(BUILD)/user/libdl.so: $(BUILD)/user/pic/libdl.o
	$(CC) -m32 -shared -nostdlib -Wl,-soname,libdl.so -Wl,--hash-style=sysv -o $@ $(BUILD)/user/pic/libdl.o

$(BUILD)/user/pic/libpthread.o: user/lib/libpthread.c user/include/pthread.h user/lib/myos.h | $(BUILD)/user/pic
	$(CC) $(UCFLAGS) -fPIC -c $< -o $@
$(BUILD)/user/libpthread.so: $(BUILD)/user/pic/libpthread.o $(BUILD)/user/libc.so
	$(CC) -m32 -shared -nostdlib -Wl,-soname,libpthread.so -Wl,--hash-style=sysv -o $@ $(BUILD)/user/pic/libpthread.o -L$(BUILD)/user -lc

# libexpat.so — Expat 2.10.0. isnan берётся из libm.
EXPAT_PARTS := xmlparse xmltok xmlrole xcs
EXPAT_PIC := $(patsubst %,$(BUILD)/user/pic/e_%.o,$(EXPAT_PARTS))
$(BUILD)/user/pic/e_%.o: user/lib/expat/%.c $(wildcard user/lib/expat/*.h) user/include/expat.h user/include/expat_external.h user/include/stdint.h | $(BUILD)/user/pic
	$(CC) $(UCFLAGS) -fPIC -Iuser/lib/expat -c $< -o $@
$(BUILD)/user/libexpat.so: $(EXPAT_PIC) $(BUILD)/user/libc.so $(BUILD)/user/libm.so
	$(CC) -m32 -shared -nostdlib -Wl,-soname,libexpat.so -Wl,--hash-style=sysv -o $@ $(EXPAT_PIC) -L$(BUILD)/user -lm -lc

# libsqlite3.so — SQLite 3.54.1. Своего VFS нет в исходнике, его даёт os_myos.c.
SQL_FLAGS := -DSQLITE_OS_OTHER=1 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_OMIT_WAL=1 -DSQLITE_OMIT_SHARED_CACHE=1 -DSQLITE_DISABLE_LFS
SQL_PARTS := sqlite3 os_myos
SQL_PIC := $(patsubst %,$(BUILD)/user/pic/s_%.o,$(SQL_PARTS))
$(BUILD)/user/pic/s_%.o: user/lib/sqlite/%.c user/include/sqlite3.h user/include/stdint.h | $(BUILD)/user/pic
	$(CC) $(UCFLAGS) -fPIC $(SQL_FLAGS) -c $< -o $@
$(BUILD)/user/libsqlite3.so: $(SQL_PIC) $(BUILD)/user/libc.so $(BUILD)/user/libm.so
	$(CC) -m32 -shared -nostdlib -Wl,-soname,libsqlite3.so -Wl,--hash-style=sysv -o $@ $(SQL_PIC) -L$(BUILD)/user -lm -lc

# libffi.so — libffi 3.8.0. Вызов по System V собирает gas, замыкание лежит в mmap.
FFI_INC := -Iuser/lib/ffi
FFI_CSRC := prep_cif types ffi closures
FFI_PIC := $(patsubst %,$(BUILD)/user/pic/f_%.o,$(FFI_CSRC))
$(BUILD)/user/pic/f_%.o: user/lib/ffi/%.c user/include/ffi.h user/include/ffitarget.h user/include/sys/mman.h $(wildcard user/lib/ffi/*.h) | $(BUILD)/user/pic
	$(CC) $(UCFLAGS) -fPIC $(FFI_INC) -c $< -o $@
$(BUILD)/user/pic/f_sysv.o: user/lib/ffi/sysv.S user/lib/ffi/fficonfig.h user/include/ffi.h user/lib/ffi/internal.h | $(BUILD)/user/pic
	$(CC) -m32 -nostdinc -fPIC $(FFI_INC) -Iuser/include -c $< -o $@
$(BUILD)/user/libffi.so: $(FFI_PIC) $(BUILD)/user/pic/f_sysv.o $(BUILD)/user/libc.so
	$(CC) -m32 -shared -nostdlib -Wl,-soname,libffi.so -Wl,--hash-style=sysv -o $@ $(FFI_PIC) $(BUILD)/user/pic/f_sysv.o -L$(BUILD)/user -lc

# OpenSSL 3.6.5. Собирает gcc хоста, без ассемблера: cc эти объекты не линкует
# (R_386_GOT32X). Сокеты включены — libcurl вешает на них HTTPS. Общую
# библиотеку собираем сами, на нашу libc.so.
OSSL_SRC := user/lib/openssl
OSSL_BLD := $(BUILD)/openssl
OSSL_FLAGS := linux-x86 no-asm no-threads no-thread-pool no-dgram no-dso no-module \
	no-apps no-docs no-tests no-engine no-quic no-comp no-afalgeng no-devcryptoeng \
	no-pinshared no-sse2 no-shared no-ui-console --with-rand-seed=devrandom \
	-std=gnu11 -ffreestanding -fno-stack-protector -fno-builtin -nostdinc -mno-sse -mno-mmx \
	-DNO_SYSLOG -DMYOS_HOST_LIBC -D__STDC_NO_ATOMICS__ \
	-I$(abspath user/include) -I$(abspath user/lib) \
	--prefix=$(abspath $(OSSL_BLD))/prefix
# Заголовки появляются уже после Configure. Отдельная отметка, чтобы ccroot
# не ждал всю компиляцию OpenSSL и не подхватывал их только на следующем make.
$(OSSL_BLD)/.configured: $(OSSL_SRC)/Configure
	@if [ -f $(OSSL_BLD)/.built ] && [ ! -f $@ ]; then touch -r $(OSSL_BLD)/.built $@; exit 0; fi
	rm -rf $(OSSL_BLD)
	mkdir -p $(OSSL_BLD)
	cd $(OSSL_BLD) && perl $(abspath $(OSSL_SRC))/Configure $(OSSL_FLAGS)
	# Configure дописывает -O3 в конец CFLAGS, поэтому более ранний -Oz он затирает.
	sed -i 's/ -O3 / $(SIZE_FLAGS) /' $(OSSL_BLD)/Makefile
	touch $@
$(OSSL_BLD)/.built: $(OSSL_BLD)/.configured
	$(MAKE) -C $(OSSL_BLD) -j$(shell nproc)
	touch $@
$(BUILD)/user/libcrypto.so: $(OSSL_BLD)/.built $(BUILD)/user/libc.so
	$(CC) -m32 -shared -nostdlib -Wl,-soname,libcrypto.so -Wl,--hash-style=sysv -Wl,-z,defs \
		-o $@ -Wl,--whole-archive $(OSSL_BLD)/libcrypto.a -Wl,--no-whole-archive -L$(BUILD)/user -lc
$(BUILD)/user/libssl.so: $(OSSL_BLD)/.built $(BUILD)/user/libcrypto.so $(BUILD)/user/libc.so
	$(CC) -m32 -shared -nostdlib -Wl,-soname,libssl.so -Wl,--hash-style=sysv -Wl,-z,defs \
		-o $@ -Wl,--whole-archive $(OSSL_BLD)/libssl.a -Wl,--no-whole-archive -L$(BUILD)/user -lcrypto -lc

# curl 8.23.0: только HTTP и HTTPS, zlib и OpenSSL. Собирает gcc хоста
# по зафиксированному curl_config.h, без configure. Объекты с R_386_GOT32X,
# поэтому cc их не линкует. .so собираем сами. curl-config — скрипт этого
# пресета: exec запускает его по #!/bin/sh.
CURL_SRC := user/lib/curl
CURL_BLD := $(BUILD)/curl
CURL_PRESET := user/lib/curl-preset
CURL_CSRC := $(filter-out %/dllmain.c,$(wildcard $(CURL_SRC)/lib/*.c) \
	$(foreach d,curlx vauth vdns vtls vquic vssh,$(wildcard $(CURL_SRC)/lib/$(d)/*.c)))
CURL_OBJS := $(patsubst $(CURL_SRC)/lib/%.c,$(CURL_BLD)/%.o,$(CURL_CSRC))
CURL_CFLAGS := -m32 -std=gnu11 -fPIC -ffreestanding -fno-stack-protector -fno-builtin \
	-nostdinc -mno-sse -mno-mmx $(SIZE_FLAGS) -Werror-implicit-function-declaration -Wno-system-headers \
	-D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -DHAVE_CONFIG_H -DBUILDING_LIBCURL \
	-I$(CURL_PRESET) -I$(CURL_SRC)/include -I$(CURL_SRC)/lib -Iuser/include -Iuser/lib \
	-isystem $(OSSL_BLD)/include -isystem $(OSSL_SRC)/include
$(CURL_BLD)/%.o: $(CURL_SRC)/lib/%.c $(CURL_PRESET)/curl_config.h $(OSSL_BLD)/.built
	@mkdir -p $(dir $@)
	$(CC) $(CURL_CFLAGS) -c $< -o $@
$(CURL_BLD)/libcurl.a: $(CURL_OBJS)
	rm -f $@ && $(AR) rcs $@ $(CURL_OBJS)
$(BUILD)/user/libcurl.so: $(CURL_BLD)/libcurl.a $(BUILD)/user/libssl.so $(BUILD)/user/libz.so $(BUILD)/user/libc.so
	$(CC) -m32 -shared -nostdlib -Wl,-soname,libcurl.so -Wl,--hash-style=sysv -Wl,-z,defs \
		-o $@ -Wl,--whole-archive $(CURL_BLD)/libcurl.a -Wl,--no-whole-archive \
		-L$(BUILD)/user -lssl -lcrypto -lz -lc

$(BUILD)/user/gzip: $(BUILD)/user/gzip.o $(BUILD)/user/zlib.o $(BUILD)/user/crt0.o $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so
	$(LD) $(DYNLDFLAGS) -o $@ $(BUILD)/user/crt0.o $(BUILD)/user/gzip.o $(BUILD)/user/zlib.o -L$(BUILD)/user -lc -lgfx

$(BUILD)/user/tar.o: user/include/zlib.h
$(BUILD)/user/tar: $(BUILD)/user/tar.o $(BUILD)/user/zlib.o $(BUILD)/user/crt0.o $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so
	$(LD) $(DYNLDFLAGS) -o $@ $(BUILD)/user/crt0.o $(BUILD)/user/tar.o $(BUILD)/user/zlib.o -L$(BUILD)/user -lc -lgfx

$(BUILD)/user/%: $(BUILD)/user/%.o $(BUILD)/user/crt0.o $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so
	$(LD) $(DYNLDFLAGS) -o $@ $(BUILD)/user/crt0.o $< -L$(BUILD)/user -lc -lgfx

# HTTPS у браузера — свой TLS 1.3 (user/lib/tls.c). Собирается gcc:
# нужна 64-битная арифметика, которой нет в cc внутри MyOS.
# wget и download ходят через libcurl, корни те же — /etc/certs.der.
TLS_OBJS := $(BUILD)/user/tls.o $(BUILD)/user/x509.o

$(BUILD)/user/tls.o: user/lib/tls.c user/lib/tls.h user/lib/x509.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/x509.o: user/lib/x509.c user/lib/x509.h user/lib/tls.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/browser.o: user/lib/tls.h

$(BUILD)/user/curlhttp.o: user/lib/curlhttp.c user/lib/curlhttp.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -I$(CURL_SRC)/include -c $< -o $@
$(BUILD)/user/wget.o: user/wget.c user/lib/curlhttp.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -I$(CURL_SRC)/include -c $< -o $@
$(BUILD)/user/download.o: user/download.c user/lib/curlhttp.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -I$(CURL_SRC)/include -c $< -o $@
$(BUILD)/user/wget: $(BUILD)/user/wget.o $(BUILD)/user/curlhttp.o $(BUILD)/user/crt0.o $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so $(BUILD)/user/libcurl.so
	$(LD) $(DYNLDFLAGS) -o $@ $(BUILD)/user/crt0.o $(BUILD)/user/wget.o $(BUILD)/user/curlhttp.o -L$(BUILD)/user -rpath-link $(BUILD)/user -lcurl -lc -lgfx
$(BUILD)/user/download: $(BUILD)/user/download.o $(BUILD)/user/curlhttp.o $(BUILD)/user/crt0.o $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so $(BUILD)/user/libcurl.so
	$(LD) $(DYNLDFLAGS) -o $@ $(BUILD)/user/crt0.o $(BUILD)/user/download.o $(BUILD)/user/curlhttp.o -L$(BUILD)/user -rpath-link $(BUILD)/user -lcurl -lc -lgfx

# JavaScript — движок user/lib/js*.c (программа js и браузер) и DOM user/lib/dom*.c (браузер).
# Тоже собирается gcc: в нём 64-битная арифметика и сопроцессор x87.
JSLIB    := js jsparse jsrun jsnum jslib jslib2 jsre
JS_OBJS  := $(patsubst %,$(BUILD)/user/l_%.o,$(JSLIB))
DOM_OBJS := $(BUILD)/user/l_dom.o $(BUILD)/user/l_domjs.o $(BUILD)/user/l_css.o

$(BUILD)/user/l_%.o: user/lib/%.c user/lib/js.h user/lib/jsint.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(DOM_OBJS) $(BUILD)/user/browser.o: user/lib/dom.h user/lib/domint.h user/lib/js.h
$(BUILD)/user/l_domjs.o: user/lib/dom_prelude.h

# прелюдия (часть окружения браузера на JS) -> строка Си
user/lib/dom_prelude.h: user/lib/dom_prelude.js
	sed -e 's/\r$$//' -e '/^ *\/\//d' -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $< > $@

$(BUILD)/user/js.o: user/lib/js.h

$(BUILD)/user/js: $(BUILD)/user/js.o $(JS_OBJS) $(BUILD)/user/crt0.o $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so
	$(LD) $(DYNLDFLAGS) -o $@ $(BUILD)/user/crt0.o $< $(JS_OBJS) -L$(BUILD)/user -lc -lgfx

# картинки (PNG, GIF, JPEG, BMP) — user/lib/img.c, пока только браузеру
$(BUILD)/user/l_img.o: user/lib/img.c user/lib/img.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/l_svg.o: user/lib/svg.c user/lib/svg.h user/lib/gfx.h user/lib/js.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/browser.o: user/lib/img.h user/lib/svg.h user/lib/browser_background.h user/lib/browser_effects.h user/lib/browser_profile.h
$(BUILD)/user/l_css.o: user/lib/css_background.h user/lib/css_effects.h

$(BUILD)/user/browser: $(BUILD)/user/browser.o $(TLS_OBJS) $(BUILD)/user/l_img.o $(BUILD)/user/l_svg.o $(JS_OBJS) $(DOM_OBJS) $(BUILD)/user/zlib.o $(BUILD)/user/crt0.o $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so
	$(LD) $(DYNLDFLAGS) -o $@ $(BUILD)/user/crt0.o $< $(TLS_OBJS) $(BUILD)/user/l_img.o $(BUILD)/user/l_svg.o $(JS_OBJS) $(DOM_OBJS) $(BUILD)/user/zlib.o -L$(BUILD)/user -lc -lgfx

# Native browser paint/scroll benchmark (not installed by default).
$(BUILD)/user/browser-paint.o: tests/browser_paint.c user/browser.c user/lib/dom.h user/lib/gfx.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/browser-paint: $(BUILD)/user/browser-paint.o $(ULIB) $(TLS_OBJS) $(BUILD)/user/l_img.o $(BUILD)/user/l_svg.o $(JS_OBJS) $(DOM_OBJS) user/user.ld
	$(LD) $(ULDFLAGS) $< $(TLS_OBJS) $(BUILD)/user/l_img.o $(BUILD)/user/l_svg.o $(JS_OBJS) $(DOM_OBJS) $(ULIB) -o $@

# cc — компилятор Си, который работает внутри MyOS (несколько файлов в user/cc/)
CC_SRC  := $(wildcard user/cc/*.c)
CC_OBJS := $(patsubst user/cc/%.c,$(BUILD)/user/cc_%.o,$(CC_SRC))

$(BUILD)/user/cc_%.o: user/cc/%.c user/cc/cc.h user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/cc: $(CC_OBJS) $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $(CC_OBJS) $(ULIB) -o $@

# cc для Linux — чтобы быстро гонять тесты компилятора (make cctest)
$(BUILD)/cc-host: $(CC_SRC) user/cc/cc.h | $(BUILD)
	$(HOSTCC) -std=gnu17 -DCC_HOST -O1 -g -Wall -Wno-unused-function -o $@ $(CC_SRC)

programs: $(UBINS)

# Библиотеки для cc — статические архивы, собранные самим cc: /lib/libc.a и
# /lib/libgfx.a на диске MyOS ($(BUILD)/mlib) и такие же для cc-host -linux ($(CCROOT)/lib).
CCROOT    := $(BUILD)/ccroot
CC_HDRS   := user/lib/myos.h user/lib/build_jobs.h user/lib/hwinfo.h user/lib/sysinfo.h user/lib/gfx.h user/lib/win.h $(wildcard user/include/*.h) $(wildcard user/include/sys/*.h) $(wildcard user/include/netinet/*.h) $(wildcard user/include/arpa/*.h) user/include/netdb.h $(wildcard $(OSSL_SRC)/include/openssl/*.h) $(wildcard $(OSSL_BLD)/include/openssl/*.h) $(wildcard $(CURL_SRC)/include/curl/*.h)
CC_LIBSRC := user/lib/libc.c user/lib/regex.c user/lib/zlib.c user/lib/gfx.c user/lib/vec.c user/lib/font.c user/lib/win.c
GFX_PARTS := gfx vec font win

$(CCROOT)/.stamp: $(CC_HDRS) $(CC_LIBSRC) $(OSSL_BLD)/.configured
	@mkdir -p $(CCROOT)/include/sys $(CCROOT)/include/netinet $(CCROOT)/include/arpa $(CCROOT)/include/openssl $(CCROOT)/include/curl $(CCROOT)/lib
	@cp user/lib/myos.h user/lib/build_jobs.h user/lib/hwinfo.h user/lib/sysinfo.h user/lib/gfx.h user/lib/win.h user/include/*.h $(CCROOT)/include/
	@cp user/include/sys/*.h $(CCROOT)/include/sys/
	@cp user/include/netinet/*.h $(CCROOT)/include/netinet/
	@cp user/include/arpa/*.h $(CCROOT)/include/arpa/
	@for f in $(OSSL_SRC)/include/openssl/*.h $(OSSL_BLD)/include/openssl/*.h; do test -f $$f || continue; cp $$f $(CCROOT)/include/openssl/; done
	@for f in $(CURL_SRC)/include/curl/*.h; do test -f $$f || continue; cp $$f $(CCROOT)/include/curl/; done
	@cp $(CC_LIBSRC) $(CCROOT)/lib/
	@touch $@

$(BUILD)/libobj-linux/%.o: user/lib/%.c $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) -linux -c $< -o $@

$(BUILD)/libobj-myos/%.o: user/lib/%.c $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) -c $< -o $@

$(BUILD)/libobj-linux/libc.o $(BUILD)/libobj-myos/libc.o: user/lib/regex.c

$(CCROOT)/lib/libc.a: $(BUILD)/libobj-linux/libc.o $(BUILD)/user/setjmp.o $(BUILD)/user/alloca.o
	rm -f $@ && $(AR) rcs $@ $^
$(CCROOT)/lib/libgfx.a: $(patsubst %,$(BUILD)/libobj-linux/%.o,$(GFX_PARTS))
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/mlib/libc.a: $(BUILD)/libobj-myos/libc.o $(BUILD)/user/setjmp.o $(BUILD)/user/alloca.o
	@mkdir -p $(@D)
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/mlib/libgfx.a: $(patsubst %,$(BUILD)/libobj-myos/%.o,$(GFX_PARTS))
	@mkdir -p $(@D)
	rm -f $@ && $(AR) rcs $@ $^
$(CCROOT)/lib/libz.a: $(BUILD)/libobj-linux/zlib.o
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/mlib/libz.a: $(BUILD)/libobj-myos/zlib.o
	@mkdir -p $(@D)
	rm -f $@ && $(AR) rcs $@ $^
MUSL_LINUX := $(patsubst user/lib/musl-math/%.c,$(BUILD)/libobj-linux/m_%.o,$(MUSL_MATH_SRC))
MUSL_MYOS  := $(patsubst user/lib/musl-math/%.c,$(BUILD)/libobj-myos/m_%.o,$(MUSL_MATH_SRC))
$(BUILD)/libobj-linux/m_%.o: user/lib/musl-math/%.c $(wildcard user/lib/musl-math/*.h) user/include/math.h user/include/fenv.h user/include/stdint.h $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) -linux -Iuser/lib/musl-math -c $< -o $@
$(BUILD)/libobj-myos/m_%.o: user/lib/musl-math/%.c $(wildcard user/lib/musl-math/*.h) user/include/math.h user/include/fenv.h user/include/stdint.h $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) -Iuser/lib/musl-math -c $< -o $@
$(CCROOT)/lib/libm.a: $(MUSL_LINUX)
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/mlib/libm.a: $(MUSL_MYOS)
	@mkdir -p $(@D)
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/libobj-linux/libdl.o: user/lib/libdl.c user/include/dlfcn.h $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) -linux -DDL_STUB -c $< -o $@
$(BUILD)/libobj-myos/libdl.o: user/lib/libdl.c user/include/dlfcn.h $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) -DDL_STUB -c $< -o $@
$(CCROOT)/lib/libdl.a: $(BUILD)/libobj-linux/libdl.o
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/mlib/libdl.a: $(BUILD)/libobj-myos/libdl.o
	@mkdir -p $(@D)
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/libobj-linux/libpthread.o $(BUILD)/libobj-myos/libpthread.o: user/include/pthread.h user/lib/myos.h
$(CCROOT)/lib/libpthread.a: $(BUILD)/libobj-linux/libpthread.o
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/mlib/libpthread.a: $(BUILD)/libobj-myos/libpthread.o
	@mkdir -p $(@D)
	rm -f $@ && $(AR) rcs $@ $^
EXPAT_LINUX := $(patsubst %,$(BUILD)/libobj-linux/e_%.o,$(EXPAT_PARTS))
EXPAT_MYOS  := $(patsubst %,$(BUILD)/libobj-myos/e_%.o,$(EXPAT_PARTS))
$(BUILD)/libobj-linux/e_%.o: user/lib/expat/%.c $(wildcard user/lib/expat/*.h) user/include/expat.h user/include/expat_external.h user/include/stdint.h $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) -linux -Iuser/lib/expat -c $< -o $@
$(BUILD)/libobj-myos/e_%.o: user/lib/expat/%.c $(wildcard user/lib/expat/*.h) user/include/expat.h user/include/expat_external.h user/include/stdint.h $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) -Iuser/lib/expat -c $< -o $@
$(CCROOT)/lib/libexpat.a: $(EXPAT_LINUX)
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/mlib/libexpat.a: $(EXPAT_MYOS)
	@mkdir -p $(@D)
	rm -f $@ && $(AR) rcs $@ $^
SQL_LINUX := $(patsubst %,$(BUILD)/libobj-linux/s_%.o,$(SQL_PARTS))
SQL_MYOS  := $(patsubst %,$(BUILD)/libobj-myos/s_%.o,$(SQL_PARTS))
$(BUILD)/libobj-linux/s_%.o: user/lib/sqlite/%.c user/include/sqlite3.h user/include/stdint.h $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) -linux $(SQL_FLAGS) -c $< -o $@
$(BUILD)/libobj-myos/s_%.o: user/lib/sqlite/%.c user/include/sqlite3.h user/include/stdint.h $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) $(SQL_FLAGS) -c $< -o $@
$(CCROOT)/lib/libsqlite3.a: $(SQL_LINUX)
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/mlib/libsqlite3.a: $(SQL_MYOS)
	@mkdir -p $(@D)
	rm -f $@ && $(AR) rcs $@ $^
FFI_LINUX := $(patsubst %,$(BUILD)/libobj-linux/f_%.o,$(FFI_CSRC))
FFI_MYOS  := $(patsubst %,$(BUILD)/libobj-myos/f_%.o,$(FFI_CSRC))
$(BUILD)/libobj-linux/f_%.o: user/lib/ffi/%.c user/include/ffi.h user/include/ffitarget.h user/include/sys/mman.h $(wildcard user/lib/ffi/*.h) $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) -linux $(FFI_INC) -c $< -o $@
$(BUILD)/libobj-myos/f_%.o: user/lib/ffi/%.c user/include/ffi.h user/include/ffitarget.h user/include/sys/mman.h $(wildcard user/lib/ffi/*.h) $(CCROOT)/.stamp $(BUILD)/cc-host
	@mkdir -p $(@D)
	$(BUILD)/cc-host -B $(CCROOT) $(FFI_INC) -c $< -o $@
$(BUILD)/ffi-sysv.o: user/lib/ffi/sysv.S user/lib/ffi/fficonfig.h user/include/ffi.h user/lib/ffi/internal.h | $(BUILD)
	$(CC) -m32 -nostdinc -fno-pie -fno-pic -D__MYOS_CC__ $(FFI_INC) -Iuser/include -c $< -o $@
$(CCROOT)/lib/libffi.a: $(FFI_LINUX) $(BUILD)/ffi-sysv.o
	rm -f $@ && $(AR) rcs $@ $^
$(BUILD)/mlib/libffi.a: $(FFI_MYOS) $(BUILD)/ffi-sysv.o
	@mkdir -p $(@D)
	rm -f $@ && $(AR) rcs $@ $^

CCROOT_READY := $(CCROOT)/.stamp $(CCROOT)/lib/libc.a $(CCROOT)/lib/libgfx.a $(CCROOT)/lib/libz.a $(CCROOT)/lib/libm.a $(CCROOT)/lib/libdl.a $(CCROOT)/lib/libpthread.a $(CCROOT)/lib/libexpat.a $(CCROOT)/lib/libsqlite3.a $(CCROOT)/lib/libffi.a
MLIBS        := $(BUILD)/mlib/libc.a $(BUILD)/mlib/libgfx.a $(BUILD)/mlib/libz.a $(BUILD)/mlib/libm.a $(BUILD)/mlib/libdl.a $(BUILD)/mlib/libpthread.a $(BUILD)/mlib/libexpat.a $(BUILD)/mlib/libsqlite3.a $(BUILD)/mlib/libffi.a

# ------------------------------------------------------------------ утилита myfs (хост)

$(BUILD)/myfs: tools/myfs.c tools/host_io.c kernel/fs.c kernel/myfs.c kernel/myfs.h kernel/block.c kernel/block.h kernel/fs.h kernel/string.c | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -c tools/host_io.c -o $(BUILD)/host_io.o
	$(HOSTCC) -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Ikernel tools/myfs.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c \
	    $(BUILD)/host_io.o -o $@

# Диск: если его нет — создаём пустой (myfs сам его отформатирует)
$(DISK):
	$(call padto,$@,$$(( $(DISK_MB) * 1048576 )))

# Установить программы в /bin (остальные файлы на диске не трогаются).
# Сторонние утилиты ставятся в /ext/bin командой ext, по каталогам /ext/instructions/<имя>.
$(BUILD)/ld.so: user/ldso.c $(BUILD)/cc-host $(CCROOT_READY)
	$(BUILD)/cc-host -B $(CCROOT) -shared -nostdlib -o $@ user/ldso.c

$(BUILD)/cert.pem: certs/roots.der tools/roots_pem.py
	python3 tools/roots_pem.py certs/roots.der $@

install: $(UBINS) $(MLIBS) $(BUILD)/myfs $(BUILD)/ld.so $(BUILD)/user/libz.so $(BUILD)/user/libm.so $(BUILD)/user/libdl.so $(BUILD)/user/libpthread.so $(BUILD)/user/libexpat.so $(BUILD)/user/libsqlite3.so $(BUILD)/user/libffi.so $(BUILD)/user/libcrypto.so $(BUILD)/user/libssl.so $(BUILD)/user/libcurl.so $(BUILD)/cert.pem | $(DISK)
	@echo "==> устанавливаю программы в /bin на $(DISK)"
	@for p in $(PROGRAMS); do $(BUILD)/myfs $(DISK) put $(BUILD)/user/$$p /bin/$$p || exit 1; done
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/gzip /bin/gunzip || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/ar /bin/ranlib || exit 1
	@echo "==> инструкции сторонних программ в /ext/instructions"
	@$(BUILD)/myfs $(DISK) mkdir /ext/bin >/dev/null
	@$(BUILD)/myfs $(DISK) mkdir /ext/instructions >/dev/null
	@$(BUILD)/myfs $(DISK) rm /ext/bin/wc || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/bin/head || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/bin/ls || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/bin/grep || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/instructions/ls.inst || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/instructions/wc.inst || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/instructions/head.inst || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/instructions/grep.inst || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/instructions/head/setup.inst || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/instructions/head/uninstall.inst || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/instructions/wc/setup.inst || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/instructions/wc/uninstall.inst || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/instructions/head || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/instructions/wc || exit 1
	@$(BUILD)/myfs $(DISK) rm /ext/instructions/lua.inst || exit 1
	@for d in ext/instructions/*; do \
	  test -d $$d || continue; \
	  name=$$(basename $$d); \
	  $(BUILD)/myfs $(DISK) mkdir /ext/instructions/$$name >/dev/null || exit 1; \
	  for f in $$d/*; do \
	    test -f $$f || continue; \
	    $(BUILD)/myfs $(DISK) put $$f /ext/instructions/$$name/$$(basename $$f) || exit 1; \
	  done; \
	done
	@echo "==> заголовки в /include, библиотеки в /lib, исходники программ в /src (для cc)"
	@for d in /include /lib /lib/fonts /src /src/cc /etc /home /root /tmp /var /var/lib /var/log /usr /usr/bin /usr/lib /usr/share /usr/share/applications /mnt /media /proc /sys /run /ext /ext/bin /ext/instructions; do $(BUILD)/myfs $(DISK) mkdir $$d >/dev/null 2>&1 || true; done
	@$(MAKE) --no-print-directory install-permissions DISK=$(DISK)
	@$(BUILD)/myfs $(DISK) mkdir /usr/share/wallpapers >/dev/null || exit 1
	@for f in wallpapers/*.jpg; do $(BUILD)/myfs $(DISK) put $$f /usr/share/wallpapers/$$(basename $$f) >/dev/null || exit 1; done
	@for f in apps/*.desktop; do $(BUILD)/myfs $(DISK) put $$f /usr/share/applications/$$(basename $$f) >/dev/null || exit 1; done
	@for f in myos.h build_jobs.h hwinfo.h sysinfo.h gfx.h win.h tls.h; do $(BUILD)/myfs $(DISK) put user/lib/$$f /include/$$f >/dev/null || exit 1; done
	@for f in user/include/*.h; do $(BUILD)/myfs $(DISK) put $$f /include/$$(basename $$f) >/dev/null || exit 1; done
	@$(BUILD)/myfs $(DISK) mkdir /include/sys >/dev/null 2>&1 || true
	@for f in user/include/sys/*.h; do $(BUILD)/myfs $(DISK) put $$f /include/sys/$$(basename $$f) >/dev/null || exit 1; done
	@$(BUILD)/myfs $(DISK) mkdir /include/netinet >/dev/null 2>&1 || true
	@for f in user/include/netinet/*.h; do $(BUILD)/myfs $(DISK) put $$f /include/netinet/$$(basename $$f) >/dev/null || exit 1; done
	@$(BUILD)/myfs $(DISK) mkdir /include/arpa >/dev/null 2>&1 || true
	@for f in user/include/arpa/*.h; do $(BUILD)/myfs $(DISK) put $$f /include/arpa/$$(basename $$f) >/dev/null || exit 1; done
	@for f in libc.c regex.c zlib.c gfx.c vec.c font.c win.c tls.c; do $(BUILD)/myfs $(DISK) put user/lib/$$f /lib/$$f >/dev/null || exit 1; done
	@for f in $(MLIBS); do $(BUILD)/myfs $(DISK) put $$f /lib/$$(basename $$f) >/dev/null || exit 1; done
	@$(BUILD)/myfs $(DISK) put $(BUILD)/ld.so /lib/ld.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libc.so /lib/libc.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libgfx.so /lib/libgfx.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libz.so /lib/libz.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libm.so /lib/libm.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libdl.so /lib/libdl.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libpthread.so /lib/libpthread.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libexpat.so /lib/libexpat.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libsqlite3.so /lib/libsqlite3.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libffi.so /lib/libffi.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libcrypto.so /lib/libcrypto.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libssl.so /lib/libssl.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put $(BUILD)/user/libcurl.so /lib/libcurl.so >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) put user/lib/curl-preset/curl-config /bin/curl-config >/dev/null || exit 1
	@$(BUILD)/myfs $(DISK) chmod 0755 /bin/curl-config || exit 1
	@$(BUILD)/myfs $(DISK) mkdir /include/openssl >/dev/null 2>&1 || true
	@for f in $(OSSL_SRC)/include/openssl/*.h $(OSSL_BLD)/include/openssl/*.h; do test -f $$f || continue; $(BUILD)/myfs $(DISK) put $$f /include/openssl/$$(basename $$f) >/dev/null || exit 1; done
	@$(BUILD)/myfs $(DISK) mkdir /include/curl >/dev/null 2>&1 || true
	@for f in $(CURL_SRC)/include/curl/*.h; do $(BUILD)/myfs $(DISK) put $$f /include/curl/$$(basename $$f) >/dev/null || exit 1; done
	@for f in user/code_*.h; do $(BUILD)/myfs $(DISK) put $$f /src/$$(basename $$f) >/dev/null || exit 1; done
	@$(BUILD)/myfs $(DISK) put user/desktop_gpu.inc /src/desktop_gpu.inc >/dev/null || exit 1
	@for f in user/*.c user/ext-auto.inc; do $(BUILD)/myfs $(DISK) put $$f /src/$$(basename $$f) >/dev/null || exit 1; done
	@for f in user/cc/*.c user/cc/cc.h; do $(BUILD)/myfs $(DISK) put $$f /src/cc/$$(basename $$f) >/dev/null || exit 1; done
	@echo "==> шрифты Ubuntu в /lib/fonts"
	@for f in fonts/*.ttf; do $(BUILD)/myfs $(DISK) put $$f /lib/fonts/$$(basename $$f) >/dev/null || exit 1; done
	@echo "==> корневые сертификаты (список Mozilla) в /etc/certs.der"
	@$(BUILD)/myfs $(DISK) mkdir /etc >/dev/null 2>&1 || true
	@$(BUILD)/myfs $(DISK) put certs/roots.der /etc/certs.der >/dev/null
	@$(BUILD)/myfs $(DISK) mkdir /etc/ssl >/dev/null
	@$(BUILD)/myfs $(DISK) put $(BUILD)/cert.pem /etc/ssl/cert.pem >/dev/null
	@$(BUILD)/myfs $(DISK) mkdir /ext/etc >/dev/null
	@$(BUILD)/myfs $(DISK) put ext/instructions/git/gitconfig /ext/etc/gitconfig >/dev/null

	@for d in /etc/services /etc/services/enabled; do $(BUILD)/myfs $(DISK) mkdir $$d >/dev/null || exit 1; done
	@for f in services/*.service; do \
	  $(BUILD)/myfs $(DISK) cat /etc/services/$$(basename $$f) >/dev/null 2>&1 || \
	  $(BUILD)/myfs $(DISK) put $$f /etc/services/$$(basename $$f) >/dev/null || exit 1; done

	@python3 tools/configure_ext_auto.py --disk "$(DISK)"
	@python3 tools/configure_ssh_keys.py --disk "$(DISK)"

# ------------------------------------------------------------------ запуск

# index=0 — primary master, с него BIOS грузит ядро. Системный том — AHCI port 0.
# Второй диск DATA_DISK остаётся IDE secondary master (/dev/sdc).
QEMU_DRIVES := -drive format=raw,file=$(BUILD)/os.img,index=0,media=disk \
               -drive if=none,id=sysdisk,file=$(DISK),format=raw \
               -device ich9-ahci,id=ahci \
               -device ide-hd,drive=sysdisk,bus=ahci.0
QEMU_CPU_FLAGS = $(if $(QEMU_ACCEL),-accel $(QEMU_ACCEL)) $(if $(QEMU_CPU),-cpu $(QEMU_CPU))
# Optional independent data disk on secondary master: make run DATA_DISK=data.img
ifneq ($(strip $(DATA_DISK)),)
QEMU_DRIVES += -drive format=raw,file=$(DATA_DISK),index=2,media=disk
endif
# auto: verified hardware VirGL on Linux/WSL, portable software framebuffer on Mac.
# GPU=on requires acceleration; GPU=off forces software; VGA=vmware/std overrides auto.
QEMU_VIDEO = python3 tools/run_qemu.py --qemu "$(QEMU)" --gpu "$(GPU)" --vga "$(VGA)" --
# сеть: карта e1000 за NAT QEMU; HOST_PORT хоста -> порт 80 MyOS (httpd)
# Другой порт: make run HOST_PORT=8081
QEMU_SSH_FORWARD :=
ifneq ($(SSH_PORT),)
QEMU_SSH_FORWARD := ,hostfwd=tcp:127.0.0.1:$(SSH_PORT)-:22
endif
QEMU_NET := -nic user,model=e1000,hostfwd=tcp::$(HOST_PORT)-:80$(QEMU_SSH_FORWARD) -rtc base=localtime

# всё, что пишет консоль ядра (и browser -v), — ещё и в build/serial.log
run: all
	$(QEMU_VIDEO) $(QEMU_CPU_FLAGS) -smp $(QEMU_CPUS) -m $(QEMU_MEM) $(QEMU_DRIVES) $(QEMU_NET) -serial file:$(BUILD)/serial.log

debug: all
	$(QEMU_VIDEO) $(QEMU_CPU_FLAGS) -smp $(QEMU_CPUS) $(QEMU_DRIVES) $(QEMU_NET) -no-reboot -d int,cpu_reset -s -S

clean:
	rm -rf $(BUILD)

cleandisk:
	rm -f $(DISK)

# Юнит-тесты (запускаются на хосте, не в QEMU)
test: | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -Wall -Wno-unused-function -Iuser/lib tests/tls_test.c -o $(BUILD)/tls_test
	./$(BUILD)/tls_test
	$(HOSTCC) -std=gnu17 -O1 -w -Iuser/lib tests/x509_test.c -o $(BUILD)/x509-test
	./$(BUILD)/x509-test
	gcc -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Ikernel tests/fs_test.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c -o $(BUILD)/fs_test
	./$(BUILD)/fs_test
	$(MAKE) $(BUILD)/myfs
	dd if=/dev/zero of=$(BUILD)/v1.img bs=512 count=4096 status=none
	gcc -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Ikernel tests/v1_seed.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c $(BUILD)/host_io.o -o $(BUILD)/v1_seed
	./$(BUILD)/v1_seed $(BUILD)/v1.img
	./$(BUILD)/myfs $(BUILD)/v1.img grow 4
	./$(BUILD)/myfs $(BUILD)/v1.img cat /keep/note | grep -q v1-ok
	./$(BUILD)/myfs $(BUILD)/v1.img ls /keep | grep -q '<LNK>'
	./$(BUILD)/myfs $(BUILD)/v1.img cat /keep/alias | grep -q v1-ok
	./$(BUILD)/myfs $(BUILD)/v1.img cat /$$(python3 -c 'print("n"*27)') | grep -q long
	python3 -c 'd=open("$(BUILD)/v1.img","rb").read(8); v=int.from_bytes(d[4:8],"little"); assert d[:4]==b"MYFS" and v==3, v'
	./$(BUILD)/myfs $(BUILD)/v1.img mkdir /$$(python3 -c 'print("d"*40)')
	./$(BUILD)/myfs $(BUILD)/v1.img mkdir /$$(python3 -c 'print("p"*180)')
	rm -f $(BUILD)/v1.img $(BUILD)/v1.img.bak
	gcc -std=gnu17 -O1 -w -fno-builtin -Ikernel -include tests/host_env.h \
	    tests/heap_test.c kernel/heap.c kernel/string.c -o $(BUILD)/heap_test
	./$(BUILD)/heap_test

# Тесты JavaScript на Linux: язык и встроенные объекты, потом DOM браузера на тестовой странице
JS_SRC := $(patsubst %,user/lib/%.c,$(JSLIB))
jstest: user/lib/dom_prelude.h | $(BUILD)
	gcc -std=gnu17 -DJS_HOST -O1 -g -Wall -Wno-unused-parameter -Wno-unused-function -Iuser/lib tests/js_host.c $(JS_SRC) -o $(BUILD)/js-host
	gcc -std=gnu17 -DJS_HOST -O1 -g -Wall -Wno-unused-parameter -Wno-unused-function -Iuser/lib tests/dom_host.c $(JS_SRC) \
	    user/lib/dom.c user/lib/domjs.c user/lib/css.c -o $(BUILD)/dom-host
	./$(BUILD)/js-host tests/js/lang.js | tee $(BUILD)/jstest.out
	@grep -q " 0 failed" $(BUILD)/jstest.out
	./$(BUILD)/js-host tests/js/gen.js > $(BUILD)/gentest.out
	@diff -u tests/js/gen.expected $(BUILD)/gentest.out && echo "generators/async: OK"
	./$(BUILD)/dom-host tests/web/t1.html click:btn click:lnk key:q:ab wait:200 > $(BUILD)/domtest.out
	@diff -u tests/web/t1.expected $(BUILD)/domtest.out && echo "DOM test: OK"
	./$(BUILD)/dom-host tests/web/async.html wait:300 > $(BUILD)/asynctest.out
	@diff -u tests/web/async.expected $(BUILD)/asynctest.out && echo "DOM async test: OK"
	./$(BUILD)/dom-host tests/web/css.html styles > $(BUILD)/csstest.out
	@diff -u tests/web/css.expected $(BUILD)/csstest.out && echo "CSS test: OK"
	python3 tests/modules_test.py

# Картинки и сертификаты на образцах (нужен python3 с Pillow и cryptography: pip install pillow cryptography)
imgtest: | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -w -Iuser/lib -DIMG_HOST tests/img_test.c user/lib/img.c user/lib/zlib.c -o $(BUILD)/img-test
	python3 tests/img/make.py $(BUILD)/imgt
	@fails=0; while read n w h t; do ./$(BUILD)/img-test $(BUILD)/imgt/$$n $(BUILD)/imgt/$$n.rgba $$w $$h $$t || fails=1; done < $(BUILD)/imgt/cases.txt; \
	    [ $$fails = 0 ] && echo "images: OK"

x509test: | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -w -Iuser/lib tests/x509_test.c -o $(BUILD)/x509-test
	python3 tests/x509/make.py $(BUILD)/x509t
	./$(BUILD)/x509-test $(BUILD)/x509t

# Весь браузер (загрузка, DOM, JS, CSS, раскладка, картинки) на Linux с AddressSanitizer:
#   build/browser-host страница.html [w 1280] [scroll] [click] [keys] [images] [timers 500] [shot f.ppm]
# Optimized host benchmark; sanitizer builds are used separately by browsertest.
$(BUILD)/browser-smooth-test: tests/browser_smooth_test.c tests/browser_host.c tests/browser_compat.h user/browser.c $(wildcard user/lib/*.h) $(JS_SRC) user/lib/dom.c user/lib/domjs.c user/lib/css.c user/lib/gfx.c user/lib/vec.c user/lib/font.c user/lib/win.c user/lib/tls.c user/lib/x509.c user/lib/img.c user/lib/zlib.c user/lib/svg.c | $(BUILD)
	gcc -m32 -std=gnu17 -O2 -g -DMYOS_HOST_LIBC -fno-builtin -nostdinc -w -Iuser/include -Iuser/lib \
	    tests/browser_smooth_test.c user/lib/win.c user/lib/gfx.c user/lib/vec.c user/lib/font.c user/lib/tls.c user/lib/x509.c \
	    user/lib/img.c user/lib/zlib.c user/lib/svg.c $(JS_SRC) user/lib/dom.c user/lib/domjs.c user/lib/css.c -o $@
.PHONY: smoothtest
smoothtest: $(BUILD)/browser-smooth-test
	./$(BUILD)/browser-smooth-test

$(BUILD)/browser-bench: tests/browser_host.c tests/browser_compat.h user/browser.c user/lib/dom_prelude.h $(wildcard user/lib/*.h) $(JS_SRC) user/lib/dom.c user/lib/domjs.c user/lib/css.c user/lib/gfx.c user/lib/vec.c user/lib/font.c user/lib/win.c user/lib/tls.c user/lib/x509.c user/lib/img.c user/lib/zlib.c user/lib/svg.c | $(BUILD)
	gcc -m32 -std=gnu17 -O2 -g -DMYOS_HOST_LIBC -fno-builtin -nostdinc -w -Iuser/include -Iuser/lib \
	    tests/browser_host.c user/lib/win.c user/lib/gfx.c user/lib/vec.c user/lib/font.c user/lib/tls.c user/lib/x509.c \
	    user/lib/img.c user/lib/zlib.c user/lib/svg.c $(JS_SRC) user/lib/dom.c user/lib/domjs.c user/lib/css.c -o $@
.PHONY: browserbench
browserbench: $(BUILD)/browser-bench
	./$(BUILD)/browser-bench tests/web/layout.html w 1920 h 600 bench 500

$(BUILD)/browser-profile-test: tests/browser_profile_host.c user/lib/browser_profile.h user/lib/tls.c user/lib/tls.h | $(BUILD)
	gcc -m32 -std=gnu17 -O1 -g -DMYOS_HOST_LIBC -ffunction-sections -fdata-sections -Iuser/lib tests/browser_profile_host.c user/lib/tls.c -Wl,--gc-sections -o $@

.PHONY: browserprofiletest
browserprofiletest: $(BUILD)/browser-profile-test
	./$(BUILD)/browser-profile-test

browsertest: user/lib/dom_prelude.h browserprofiletest | $(BUILD)
	gcc -m32 -std=gnu17 -O1 -g -DMYOS_HOST_LIBC -fsanitize=address,undefined -fno-omit-frame-pointer -fno-builtin -nostdinc -w -Iuser/include -Iuser/lib \
	    tests/browser_host.c user/lib/win.c user/lib/gfx.c user/lib/vec.c user/lib/font.c user/lib/tls.c user/lib/x509.c \
	    user/lib/img.c user/lib/zlib.c user/lib/svg.c $(JS_SRC) user/lib/dom.c user/lib/domjs.c user/lib/css.c -o $(BUILD)/browser-host
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host tests/web/t1.html scroll w 1280 scroll w 480 click keys timers 200
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host tests/web/css.html scroll w 1000 images
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host --clipboard-test
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host --compat-test
	ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host tests/web/modules.html assert-no-errors scroll
	UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host tests/web/async-generators.html timers 150 assert-no-errors
	UBSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)/browser-host tests/web/web-api.html timers 250 assert-no-errors assert-text web-api-ok

# Сглаженная графика и шрифты на Linux: пробная картинка build/gfxtest.ppm
gfxtest: | $(BUILD)
	gcc -std=gnu17 -m32 -O1 -g -fno-builtin -nostdinc -w -Iuser/lib tests/gfx_host.c user/lib/gfx.c user/lib/vec.c \
	    user/lib/font.c -o $(BUILD)/gfx-host
	./$(BUILD)/gfx-host fonts > $(BUILD)/gfxtest.ppm && echo "==> $(BUILD)/gfxtest.ppm"

# Тесты компилятора cc на Linux: тестовые программы и «cc собирает сам себя»
cctest: $(BUILD)/cc-host $(CCROOT_READY)
	python3 tests/cc_crlf.py $(BUILD)/cc-host $(CCROOT)
	@echo 'int main(void) { return 0; }' > $(BUILD)/cct_probe.c
	@$(BUILD)/cc-host -B $(CCROOT) -linux $(BUILD)/cct_probe.c -o $(BUILD)/cct_probe
	@if ! ./$(BUILD)/cct_probe 2>/dev/null; then \
	    echo "32-bit programs cannot run here - only checking that tests and cc itself compile"; \
	    for t in tests/cc/*.c; do $(BUILD)/cc-host -B $(CCROOT) -linux $$t -o $(BUILD)/cct_x || exit 1; done; \
	    for t in tests/cc/*.c; do $(BUILD)/cc-host -B $(CCROOT) -linux -fPIC $$t -o $(BUILD)/cct_x || exit 1; done; \
	    $(BUILD)/cc-host -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc1 && \
	    sh tests/ccshared.sh $(BUILD)/cc-host $(CCROOT) && echo "compile-only: OK"; \
	    exit 0; \
	fi; \
	for t in tests/cc/*.c; do \
	    n=$$(basename $$t .c); \
	    $(BUILD)/cc-host -B $(CCROOT) -linux $$t -o $(BUILD)/cct_$$n && ./$(BUILD)/cct_$$n || exit 1; \
	done; \
	for t in tests/cc/*.c; do \
	    n=$$(basename $$t .c); \
	    $(BUILD)/cc-host -B $(CCROOT) -linux -fPIC $$t -o $(BUILD)/cct_pic_$$n && ./$(BUILD)/cct_pic_$$n || exit 1; \
	done; \
	echo "cc -fPIC: OK"; \
	rm -f $(BUILD)/cct_cc1 $(BUILD)/cct_cc2 $(BUILD)/cct_cc3; \
	$(BUILD)/cc-host -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc1 && \
	./$(BUILD)/cct_cc1 -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc2 && \
	./$(BUILD)/cct_cc2 -B $(CCROOT) -linux $(CC_SRC) -o $(BUILD)/cct_cc3 && \
	cmp $(BUILD)/cct_cc2 $(BUILD)/cct_cc3 && echo "cc compiles itself: OK (stage 2 == stage 3)" && \
	sh tests/cclink.sh $(BUILD)/cc-host $(CCROOT) && sh tests/cclink.sh $(BUILD)/cct_cc2 $(CCROOT) && \
	sh tests/cclib.sh $(BUILD)/cc-host $(CCROOT) && sh tests/cclib.sh $(BUILD)/cct_cc2 $(CCROOT) && \
	sh tests/ccshared.sh $(BUILD)/cc-host $(CCROOT) && sh tests/ccshared.sh $(BUILD)/cct_cc2 $(CCROOT)

vfstest: all
	python3 tests/vfs_smoke.py

largetest: | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -c tools/host_io.c -o $(BUILD)/large-host-io.o
	$(HOSTCC) -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Ikernel tests/large_fs_test.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c $(BUILD)/large-host-io.o -o $(BUILD)/large-fs-test
	python3 tests/run_large_fs.py $(BUILD)/large-fs-test

# Lua 5.4.8 тем же cc, которым его соберёт ext install lua внутри системы.
luatest: $(BUILD)/cc-host $(CCROOT_READY)
	@sh tests/lua.sh $(BUILD)/cc-host $(CCROOT)

# ext без сети: разбор инструкций, каталоги, отказ на неизвестной команде.
exttest: $(BUILD)/cc-host $(CCROOT_READY) $(BUILD)/user/download $(BUILD)/user/ext
	@$(BUILD)/cc-host -B $(CCROOT) -linux user/ext.c -o $(BUILD)/ext-linux
	@./$(BUILD)/ext-linux --selftest

# libpthread: cc принимает pthread.h, -lpthread находит .so. Сами потоки — только в госте.
pthreadtest: $(BUILD)/cc-host $(CCROOT_READY) $(BUILD)/user/libpthread.so $(BUILD)/mlib/libpthread.a
	@$(BUILD)/cc-host -B $(CCROOT) -c tests/pthread_test.c -o $(BUILD)/pthread-test.o
	@$(BUILD)/cc-host -B $(CCROOT) -linux -L $(BUILD)/user tests/pthread_test.c -lpthread -o $(BUILD)/pthread-link
	@readelf -d $(BUILD)/pthread-link | grep -q libpthread.so

# libdl: -ldl находит библиотеку. Без загрузчика dlopen возвращает ошибку.
dltest: $(BUILD)/cc-host $(CCROOT_READY)
	@$(BUILD)/cc-host -B $(CCROOT) -linux tests/dl_test.c -ldl -o $(BUILD)/dl-test
	@./$(BUILD)/dl-test

# libexpat: короткий документ разбирается, битый XML даёт ошибку.
expattest: $(BUILD)/cc-host $(CCROOT_READY) $(BUILD)/user/libexpat.so
	@$(BUILD)/cc-host -B $(CCROOT) -linux tests/expat.c -lexpat -o $(BUILD)/expat-test
	@./$(BUILD)/expat-test
	@readelf -d $(BUILD)/user/libexpat.so | grep -q libexpat.so

# libsqlite3: строка переживает закрытие базы и читается снова.
sqlitetest: $(BUILD)/cc-host $(CCROOT_READY) $(BUILD)/user/libsqlite3.so
	@$(BUILD)/cc-host -B $(CCROOT) -linux tests/sqlite.c -lsqlite3 -o $(BUILD)/sqlite-test
	@./$(BUILD)/sqlite-test
	@readelf -d $(BUILD)/user/libsqlite3.so | grep -q libsqlite3.so

# libffi: вызов int и double, плюс замыкание в исполняемой странице.
ffitest: $(BUILD)/cc-host $(CCROOT_READY) $(BUILD)/user/libffi.so
	@$(BUILD)/cc-host -B $(CCROOT) -linux tests/ffi.c -lffi -o $(BUILD)/ffi-test
	@./$(BUILD)/ffi-test
	@readelf -d $(BUILD)/user/libffi.so | grep -q libffi.so

# SHA-256 «abc» через libcrypto. Тест линкует gcc: в объектах OpenSSL есть R_386_GOT32X.
ossltest: $(BUILD)/user/libcrypto.so $(BUILD)/user/libssl.so $(CCROOT)/lib/libc.a
	@$(CC) -m32 -nostdlib -nostdinc -no-pie -ffreestanding -fno-stack-protector -fno-builtin -mno-sse -mno-mmx -std=gnu11 \
		-I$(OSSL_BLD)/include -I$(OSSL_SRC)/include -Iuser/include -Iuser/lib \
		tests/sha256.c $(OSSL_BLD)/libcrypto.a $(CCROOT)/lib/libc.a -o $(BUILD)/sha256-test
	@./$(BUILD)/sha256-test
	@readelf -d $(BUILD)/user/libcrypto.so | grep -q libcrypto.so
	@readelf -d $(BUILD)/user/libssl.so | grep -q libssl.so

# GET http://127.0.0.1 и https://127.0.0.1. Это не гостевой httpd: его нет, пока виртуальная машина держит диск.
curltest: $(BUILD)/user/libcurl.so $(CCROOT)/lib/libc.a $(CCROOT)/lib/libz.a
	@$(CC) -m32 -nostdlib -nostdinc -no-pie -ffreestanding -fno-stack-protector -fno-builtin -mno-sse -mno-mmx -std=gnu11 \
		-D_POSIX_C_SOURCE=200809L -I$(CURL_SRC)/include -Iuser/include -Iuser/lib \
		tests/curlget.c $(CURL_BLD)/libcurl.a $(OSSL_BLD)/libssl.a $(OSSL_BLD)/libcrypto.a \
		$(CCROOT)/lib/libz.a $(CCROOT)/lib/libc.a -o $(BUILD)/curlget-test
	@python3 tests/curl_test.py $(BUILD)/curlget-test
	@readelf -d $(BUILD)/user/libcurl.so | grep -q libcurl.so

# libm: pow находится и без -lm, потому что cc подключает libm сам.
lmtest: $(BUILD)/cc-host $(CCROOT_READY)
	@$(BUILD)/cc-host -B $(CCROOT) -linux tests/cc/libm.c -o $(BUILD)/lm-libc
	@./$(BUILD)/lm-libc
	@$(BUILD)/cc-host -B $(CCROOT) -linux tests/cc/libm.c -lm -o $(BUILD)/lm-test
	@./$(BUILD)/lm-test
	@$(BUILD)/cc-host -B $(CCROOT) -linux tests/libm_full.c -lm -o $(BUILD)/lm-full
	@./$(BUILD)/lm-full

# libz: round-trip, куски входа и повтор после deflateReset.
ztest: $(BUILD)/cc-host $(CCROOT_READY)
	@$(BUILD)/cc-host -B $(CCROOT) -linux tests/zlib_test.c -lz -o $(BUILD)/zlib-test
	@./$(BUILD)/zlib-test

# gzip на хосте: тот же cc, что работает внутри системы, плюс сверка с GNU gzip.
gztest: $(BUILD)/cc-host $(CCROOT_READY)
	@$(BUILD)/cc-host -B $(CCROOT) -linux user/gzip.c -lz -o $(BUILD)/gzip-linux
	@sh tests/gzip.sh $(BUILD)/gzip-linux

# tar на хосте: тот же cc, синтетические архивы и настоящий исходник zlib.
tartest: $(BUILD)/cc-host $(CCROOT_READY)
	@$(BUILD)/cc-host -B $(CCROOT) -linux user/tar.c -lz -o $(BUILD)/tar-linux
	@sh tests/tar.sh $(BUILD)/tar-linux

# Небольшой make: правила, шаблоны, переменные. Тот же cc, что внутри системы.
maketest: $(BUILD)/cc-host $(CCROOT_READY)
	@$(BUILD)/cc-host -B $(CCROOT) -linux user/make.c -o $(BUILD)/make-linux
	@sh tests/make.sh $(BUILD)/make-linux
	@python3 tests/make_parallel.py $(BUILD)/make-linux
	@$(BUILD)/cc-host -B $(CCROOT) -linux tests/build_jobs_test.c -o $(BUILD)/build-jobs-test
	@./$(BUILD)/build-jobs-test

cptest: $(BUILD)/cc-host $(CCROOT_READY)
	@$(BUILD)/cc-host -B $(CCROOT) -linux user/cp.c -o $(BUILD)/cp-linux
	@sh tests/cp.sh $(BUILD)/cp-linux

lstest: $(BUILD)/cc-host $(CCROOT_READY)
	@$(BUILD)/cc-host -B $(CCROOT) -linux user/ls.c -o $(BUILD)/ls-linux
	@sh tests/ls.sh $(BUILD)/ls-linux

rmtest: $(BUILD)/cc-host $(CCROOT_READY)
	@$(BUILD)/cc-host -B $(CCROOT) -linux user/rm.c -o $(BUILD)/rm-linux
	@sh tests/rm.sh $(BUILD)/rm-linux

$(BUILD)/user/gpuscene.o: tests/gpu_scene.c user/desktop.c user/desktop_gpu.inc user/lib/gfx.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/gpuscene: $(BUILD)/user/gpuscene.o $(BUILD)/user/l_img.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(BUILD)/user/l_img.o $(ULIB) -o $@

$(BUILD)/user/windowframecheck.o: tests/window_framecheck.c user/desktop.c user/desktop_gpu.inc user/lib/win.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/windowframecheck: $(BUILD)/user/windowframecheck.o $(BUILD)/user/l_img.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(BUILD)/user/l_img.o $(ULIB) -o $@

$(BUILD)/user/mousecheck.o: tests/mousecheck.c user/lib/gfx.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/mousecheck: $(BUILD)/user/mousecheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

$(BUILD)/user/inputlatency.o: tests/inputlatency.c user/lib/gfx.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/inputlatency: $(BUILD)/user/inputlatency.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

.PHONY: mousetest
mousetest:
	python3 tests/mouse_smoke.py

.PHONY: graphicstest
graphicstest:
	python3 tests/graphics_launcher_test.py
	python3 tests/graphics_smoke.py --backend gpu
	python3 tests/graphics_smoke.py --backend std
	python3 tests/graphics_smoke.py --backend virtio

$(BUILD)/user/gpucheck.o: tests/gpucheck.c user/lib/gfx.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/gpucheck: $(BUILD)/user/gpucheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

# Загрузчик и программа с .so: внутри QEMU, диск — копия.
dynsystest: $(BUILD)/ld.so $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so $(BUILD)/user/echo $(BUILD)/user/sh $(BUILD)/cc-host $(CCROOT_READY) $(BUILD)/os.img $(BUILD)/myfs
	gcc -m32 -shared -fPIC -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -mno-sse -mno-mmx -O2 \
	    -Wl,-soname,libgccadd.so -Wl,--hash-style=sysv -o $(BUILD)/libgccadd.so tests/dyn/gccadd.c
	$(BUILD)/cc-host -B $(CCROOT) -nostdlib -o $(BUILD)/cc-gccso tests/dyn/use_gcc.c $(BUILD)/libgccadd.so
	$(BUILD)/cc-host -B $(CCROOT) -shared -nostdlib -o $(BUILD)/libadd.so tests/ldso/add.c
	$(NASM) -f elf32 tests/dyn/start.asm -o $(BUILD)/dynstart.o
	gcc -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-builtin -nostdlib -nostdinc -mno-sse -mno-mmx -O2 \
	    -c tests/dyn/use_cc.c -o $(BUILD)/use_cc.o
	ld -m elf_i386 -nostdlib -dynamic-linker /lib/ld.so --hash-style=sysv -Ttext-segment=0x40000000 \
	    -o $(BUILD)/gcc-ccso $(BUILD)/dynstart.o $(BUILD)/use_cc.o $(BUILD)/libadd.so
	gcc -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-builtin -nostdlib -nostdinc -mno-sse -mno-mmx -O2 \
	    -Iuser/include -Iuser/lib -c tests/dyn/gfxuse.c -o $(BUILD)/gfxuse.o
	ld $(DYNLDFLAGS) -o $(BUILD)/gfxuse $(BUILD)/user/crt0.o $(BUILD)/gfxuse.o -L$(BUILD)/user -lc -lgfx
	python3 tests/dynsys_smoke.py

ldsotest: $(BUILD)/ld.so $(BUILD)/cc-host $(CCROOT_READY) $(BUILD)/os.img $(BUILD)/myfs
	$(BUILD)/cc-host -B $(CCROOT) -shared -nostdlib -o $(BUILD)/libadd.so tests/ldso/add.c
	$(BUILD)/cc-host -B $(CCROOT) -shared -nostdlib -o $(BUILD)/libb.so tests/ldso/b.c $(BUILD)/libadd.so
	$(BUILD)/cc-host -B $(CCROOT) -nostdlib -o $(BUILD)/dynuse tests/ldso/main.c $(BUILD)/libadd.so $(BUILD)/ld.so
	python3 tests/ldso_smoke.py

.PHONY: all run debug clean cleandisk test programs install cctest luatest jstest gfxtest vfstest largetest exttest gztest tartest maketest cptest lstest rmtest ldsotest dynsystest
.SECONDARY:



$(BUILD)/user/servicecheck.o: tests/servicecheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/user/servicecheck: $(BUILD)/user/servicecheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

.PHONY: servicetest
servicetest:
	python3 tests/services_smoke.py

.PHONY: codetest
codetest: | $(BUILD)
	$(HOSTCC) -std=gnu11 -g -O1 -Wall -Wextra -fsanitize=address,undefined tests/code_features.c -o $(BUILD)/code-features-test
	./$(BUILD)/code-features-test user/lib/myos.h user/lib/gfx.h user/lib/win.h
	$(HOSTCC) -std=gnu11 -DMYOS_HOST_LIBC -g -O1 -fsanitize=address,undefined -fno-builtin -ffunction-sections -fdata-sections -Iuser/lib tests/code_document.c -Wl,--gc-sections -o $(BUILD)/code-document-test
	./$(BUILD)/code-document-test

# Settings validates images; the desktop decodes the saved wallpaper.
$(BUILD)/user/desktop.o: user/desktop_gpu.inc

$(BUILD)/user/settings $(BUILD)/user/desktop: $(BUILD)/user/%: $(BUILD)/user/%.o $(BUILD)/user/l_img.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(BUILD)/user/l_img.o $(ULIB) -o $@

.PHONY: settingstest
settingstest:
	python3 tests/settings_smoke.py

$(BUILD)/user/sysinfocheck.o: tests/sysinfocheck.c user/lib/myos.h user/lib/sysinfo.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/sysinfocheck: $(BUILD)/user/sysinfocheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
.PHONY: sysinfotest
sysinfotest:
	python3 tests/build_info_test.py
	python3 tests/sysinfo_smoke.py

# Hardware/USB live build is isolated from the original QEMU boot image.
$(HWBUILD)/virtgpu.o: kernel/virtgpu_render.inc kernel/virtgpu_cursor.inc
$(BUILD)/e1000.o $(HWBUILD)/e1000.o: kernel/e1000_pch.inc
EMU_SRC := $(wildcard drivers/video/x86emu/*.c)
EMU_OBJS := $(patsubst drivers/video/x86emu/%.c,$(HWBUILD)/x86emu/%.o,$(EMU_SRC))
$(HWBUILD)/x86emu/%.o: drivers/video/x86emu/%.c drivers/video/x86emu/include/*.h drivers/video/emu_port.h drivers/video/emu_io.h
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -w -c $< -o $@
HWOBJS := $(HWBUILD)/entry.o $(HWBUILD)/isr.o $(HWBUILD)/ap.o $(patsubst kernel/%.c,$(HWBUILD)/%.o,$(C_SRC)) $(patsubst drivers/video/%.c,$(HWBUILD)/video/%.o,$(VIDEO_SRC))
$(HWOBJS): user/lib/hwinfo.h
$(HWBUILD):
	mkdir -p $@
$(HWBUILD)/%.o: kernel/%.c kernel/*.h $(BUILD)/build_info.h | $(HWBUILD)
	$(CC) $(CFLAGS) -DMYOS_HARDWARE -c $< -o $@
$(HWBUILD)/%.o: kernel/%.asm | $(HWBUILD)
	$(NASM) -f elf32 $< -o $@
$(HWBUILD)/kernel.elf: $(HWOBJS) $(EMU_OBJS) boot/hardware.ld
	$(LD) -m elf_i386 -T boot/hardware.ld -nostdlib --gc-sections $(HWOBJS) $(EMU_OBJS) $(shell $(CC) -m32 -print-libgcc-file-name) -o $@
$(HWBUILD)/kernel.bin: $(HWBUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
$(BUILD)/user/consoleload.o: tests/console_load.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/consoleload: $(BUILD)/user/consoleload.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(BUILD)/user/libmyos.a -o $@
.PHONY: usb-image usbtest consoletest consoleloadtest ethernettest ethernet-smoke dhcptest
dhcptest: | $(BUILD)
	$(HOSTCC) -m32 -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-builtin -ffunction-sections -fdata-sections tests/dhcp_client.c -Wl,--gc-sections -o $(BUILD)/dhcp-client-test
	./$(BUILD)/dhcp-client-test
ethernet-smoke: usb-image
	python3 tests/ethernet_smoke.py
ethernettest: | $(BUILD)
	$(HOSTCC) -m32 -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-builtin tests/e1000_driver.c -o $(BUILD)/e1000-driver-test
	./$(BUILD)/e1000-driver-test
consoleloadtest: usb-image $(BUILD)/user/consoleload
	python3 tests/console_load_smoke.py
consoletest: | $(BUILD)
	$(HOSTCC) -m32 -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-builtin tests/console_output.c -o $(BUILD)/console-output-test
	./$(BUILD)/console-output-test
usb-image: $(HWBUILD)/kernel.bin $(UBINS) $(BUILD)/myfs
	python3 tools/make_usb_image.py --nasm $(NASM) --nm "$(NM)" --video "$(USB_VIDEO)" --root-mb "$(USB_ROOT_MB)"
usbtest: usb-image
	python3 tests/hardware_smoke.py

.PHONY: htoptest
htoptest:
	$(MAKE) build/os.img install DISK=build/htop-test.img
	python3 tests/htop_smoke.py

$(BUILD)/user/cpuinfocheck.o: tests/cpuinfocheck.c user/lib/myos.h user/lib/sysinfo.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/cpuinfocheck: $(BUILD)/user/cpuinfocheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@

$(BUILD)/user/terminalresizecheck.o: tests/terminalresizecheck.c user/terminal.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/terminalresizecheck: $(BUILD)/user/terminalresizecheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
.PHONY: terminalresizetest
terminalresizetest:
	$(MAKE) build/os.img build/user/terminalresizecheck install DISK=build/terminal-resize-test.img
	python3 tests/terminal_resize_smoke.py

$(BUILD)/user/terminalwindowcheck.o: tests/terminalwindowcheck.c user/lib/myos.h user/lib/win.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/terminalwindowcheck: $(BUILD)/user/terminalwindowcheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@



.PHONY: terminalwindowtest
terminalwindowtest:
	@mkdir -p build
	python3 -c "from pathlib import Path; p = Path('build/terminal-resize-test.img'); f = p.open('wb'); f.truncate(128 * 1024 * 1024); f.close()"
	$(MAKE) build/os.img build/user/terminalwindowcheck install DISK=build/terminal-resize-test.img
	python3 tests/terminal_window_smoke.py

# Mouse wheel: terminal history on host + PS/2 packets in QEMU.
.PHONY: wheeltest
wheeltest: $(BUILD)/os.img
	$(HOSTCC) -m32 -std=gnu11 -DMYOS_HOST_LIBC -g -O1 -fsanitize=address,undefined -fno-builtin -ffunction-sections -fdata-sections -Iuser/lib tests/terminal_history.c -Wl,--gc-sections -o $(BUILD)/terminal-history-test
	./$(BUILD)/terminal-history-test
	python3 tests/wheel_smoke.py

# Native user threads and shared-address-space SMP regression tests.
$(BUILD)/user/threadcheck.o: tests/threadcheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/threadcheck: $(BUILD)/user/threadcheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
.PHONY: threadtest
threadtest:
	python3 tests/thread_smoke.py

.PHONY: klogtest
klogtest: | $(BUILD)
	$(HOSTCC) -m32 -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-builtin tests/klog_test.c -o $(BUILD)/klog-test
	./$(BUILD)/klog-test

.PHONY: dmesgtest
dmesgtest: | $(BUILD)
	$(HOSTCC) -m32 -std=gnu11 -DMYOS_HOST_LIBC -g -O1 -fsanitize=address,undefined -fno-builtin -Iuser/lib tests/dmesg_test.c -o $(BUILD)/dmesg-test
	./$(BUILD)/dmesg-test

$(BUILD)/user/klogcheck.o: tests/klogcheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/klogcheck: $(BUILD)/user/klogcheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
.PHONY: klog-smoke
klog-smoke: usb-image $(BUILD)/user/klogcheck
	python3 tests/klog_smoke.py

.PHONY: videodrivertest videotest
videodrivertest: | $(BUILD)
	$(HOSTCC) -m32 -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-builtin tests/video_driver.c -o $(BUILD)/video-driver-test
	./$(BUILD)/video-driver-test
videotest: usb-image videodrivertest
	python3 tests/video_smoke.py

.PHONY: videoruntimetest
videoruntimetest: usb-image
	python3 tests/video_runtime_smoke.py

.PHONY: usbmousetest usbmouse-smoke
usbmousetest: | $(BUILD)
	$(HOSTCC) -m32 -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-builtin -ffunction-sections -fdata-sections tests/usb_mouse_test.c -Wl,--gc-sections -o $(BUILD)/usb-mouse-test
	./$(BUILD)/usb-mouse-test
usbmouse-smoke: usb-image usbmousetest
	python3 tests/usb_mouse_smoke.py

.PHONY: nvidia-iotest
nvidia-iotest: | $(BUILD)
	$(HOSTCC) -m32 -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-builtin -ffunction-sections -fdata-sections tests/nvidia_io_test.c -Wl,--gc-sections -o $(BUILD)/nvidia-io-test
	./$(BUILD)/nvidia-io-test

# OpenSSH ext port; host build uses the same compiler and static libc as ext.
$(BUILD)/file.o $(HWBUILD)/file.o: kernel/ssh-local.inc
$(BUILD)/syscall.o $(HWBUILD)/syscall.o: kernel/ssh-syscall.inc
.PHONY: openssh-build openssh-recipetest openssh-tests-ready opensshtest openssh-exttest
openssh-build: $(BUILD)/cc-host $(CCROOT)/.stamp $(BUILD)/mlib/libc.a
	python3 tests/openssh_build.py
openssh-recipetest: $(BUILD)/cc-host $(CCROOT_READY)
	$(BUILD)/cc-host -B $(CCROOT) -linux user/make.c -o $(BUILD)/make-linux
	python3 tests/openssh_recipe.py
openssh-tests-ready: openssh-build openssh-recipetest fslookuptest
	$(BUILD)/cc-host -B $(BUILD)/openssh-native/sysroot -I$(BUILD)/openssh-native/openssh-10.6p2 -I$(BUILD)/openssh-native/openssh-10.6p2/myos-include -I$(BUILD)/openssh-native/openssh-10.6p2/openbsd-compat tests/sshcheck.c $(BUILD)/openssh-native/openssh-10.6p2/myos-posix.o $(BUILD)/openssh-native/openssh-10.6p2/c0.a $(BUILD)/openssh-native/openssh-10.6p2/c1.a -o $(BUILD)/openssh-native/sshcheck
opensshtest: openssh-tests-ready
	python3 tests/openssh_smoke.py
openssh-exttest: openssh-tests-ready
	python3 tests/openssh_smoke.py --ext
	python3 tests/openssh_service.py

$(BUILD)/fs-lookup-test: tests/fs_lookup_test.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c $(BUILD)/myfs
	$(HOSTCC) -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -Ikernel tests/fs_lookup_test.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c $(BUILD)/host_io.o -o $@
.PHONY: fslookuptest
fslookuptest: $(BUILD)/fs-lookup-test
	python3 -c 'import tempfile,pathlib,subprocess; t=tempfile.TemporaryDirectory(prefix="myos-lookup-"); p=pathlib.Path(t.name)/"disk.img"; f=p.open("wb"); f.truncate(64*1024*1024); f.close(); subprocess.run(["$(BUILD)/fs-lookup-test",str(p)],check=True)'

$(BUILD)/file.o $(HWBUILD)/file.o: kernel/fs.h
$(BUILD)/fs.o $(HWBUILD)/fs.o: kernel/fs-permissions.inc
$(BUILD)/myfs.o $(HWBUILD)/myfs.o: kernel/myfs-permissions.inc
$(BUILD)/syscall.o $(HWBUILD)/syscall.o: kernel/fs-syscall.inc
$(BUILD)/myfs: kernel/fs-permissions.inc kernel/myfs-permissions.inc

$(BUILD)/fs-permissions-test: tests/fs_permissions_test.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c kernel/fs-permissions.inc kernel/myfs-permissions.inc $(BUILD)/myfs
	$(HOSTCC) -std=gnu17 -O1 -w -fno-builtin -DHOST_TEST -DFS_PERMISSIONS_TEST -Ikernel tests/fs_permissions_test.c kernel/fs.c kernel/myfs.c kernel/block.c kernel/string.c $(BUILD)/host_io.o -o $@
.PHONY: permissionstest
permissionstest: $(BUILD)/fs-permissions-test
	python3 tests/fs_permissions.py

.PHONY: permissions-smoke
permissions-smoke:
	python3 tests/permissions_smoke.py

.PHONY: install-permissions
install-permissions: $(BUILD)/myfs
	@$(BUILD)/myfs $(DISK) chmod 0700 /root || exit 1
	@$(BUILD)/myfs $(DISK) chmod 1777 /tmp || exit 1
	@for key in /etc/ssh/ssh_host_ed25519_key /root/.ssh/id_ed25519; do \
	  if $(BUILD)/myfs $(DISK) stat $$key >/dev/null 2>&1; then \
	    $(BUILD)/myfs $(DISK) chown 0 0 $$key && $(BUILD)/myfs $(DISK) chmod 0600 $$key || exit 1; \
	  fi; \
	done

.PHONY: isolationtest
isolationtest:
	python3 tests/isolation_smoke.py

$(BUILD)/user/live-rootcheck.o: tests/live_rootcheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/live-rootcheck: $(BUILD)/user/live-rootcheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
.PHONY: liveroottest
liveroottest: usb-image $(BUILD)/user/live-rootcheck
	python3 tests/live_root_smoke.py

# Network development boot: publish versioned images, then keep the HTTP server running.
.PHONY: envtest netboot-image netboot-serve netboot-usb netboottest netbootsmoke netboot-usb-smoke
netboot-image: usb-image
	python3 tools/netboot.py publish
netboot-serve:
	python3 tools/netboot.py serve --bind "$(NETBOOT_BIND)" --port "$(NETBOOT_PORT)"
netboot-usb:
	python3 tools/build_ipxe.py --url "$(NETBOOT_URL)"
netboottest:
	python3 tests/netboot_test.py

netbootsmoke: netboot-image
	python3 tests/netboot_smoke.py

netboot-usb-smoke:
	python3 tests/netboot_smoke.py --bootstrap

envtest:
	python3 tests/env_config_test.py

# Disk-cache eviction, close error propagation and AHCI DMA recovery.
.PHONY: storage-recoverytest
storage-recoverytest: | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -g -fno-builtin -DHOST_TEST -Ikernel tests/block_cache_test.c kernel/block.c kernel/string.c -o $(BUILD)/block-cache-test
	./$(BUILD)/block-cache-test
	$(HOSTCC) -m32 -std=gnu17 -O1 -g -fno-builtin -ffunction-sections -fdata-sections tests/close_flush_test.c -Wl,--gc-sections -o $(BUILD)/close-flush-test
	./$(BUILD)/close-flush-test
	$(HOSTCC) -m32 -std=gnu17 -O1 -g -fno-builtin -ffunction-sections -fdata-sections tests/ahci_recovery_test.c kernel/string.c -Wl,--gc-sections -o $(BUILD)/ahci-recovery-test
	./$(BUILD)/ahci-recovery-test

$(BUILD)/user/storagecheck.o: tests/storagecheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/storagecheck: $(BUILD)/user/storagecheck.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
.PHONY: storage-smoketest
storage-smoketest: storage-recoverytest
	python3 tests/storage_smoke.py

.PHONY: desktopdragtest
desktopdragtest: | $(BUILD)
	$(HOSTCC) -m32 -std=gnu11 -O1 -g -fno-builtin -ffunction-sections -fdata-sections -fsanitize=address,undefined -Iuser/include -Iuser/lib tests/desktop_drag_test.c -Wl,--gc-sections -o $(BUILD)/desktop-drag-test
	./$(BUILD)/desktop-drag-test

$(BUILD)/user/dragcheck.o: tests/desktop_dragcheck.c user/desktop.c user/desktop_gpu.inc user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/dragcheck: $(BUILD)/user/dragcheck.o $(BUILD)/user/l_img.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(BUILD)/user/l_img.o $(ULIB) -o $@

.PHONY: desktopdrag-smoke
desktopdrag-smoke: usb-image $(BUILD)/os.img $(BUILD)/user/dragcheck
	python3 tests/desktop_drag_smoke.py

$(BUILD)/user/ext.o: user/ext-auto.inc

$(BUILD)/user/extautolock.o: tests/ext_auto_lock.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/extautolock: $(BUILD)/user/extautolock.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
.PHONY: extautotest
extautotest: $(BUILD)/myfs
	python3 tests/ext_auto_config_test.py
	python3 tests/ext_auto_smoke.py

.PHONY: sshkeytest
sshkeytest: $(BUILD)/myfs
	python3 tests/ssh_key_config_test.py

.PHONY: ptytest
ptytest: | $(BUILD)
	$(HOSTCC) -m32 -std=gnu11 -O1 -g -fno-builtin -ffunction-sections -fdata-sections -fsanitize=address,undefined tests/pty_output.c kernel/string.c -Wl,--gc-sections -o $(BUILD)/pty-output-test
	./$(BUILD)/pty-output-test

$(BUILD)/user/nanocheck.o: tests/nanocheck.c user/lib/myos.h | $(BUILD)/user
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/user/nanocheck: $(BUILD)/user/nanocheck.o $(BUILD)/user/libmyos.a user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
.PHONY: nano-build nanotest
nano-build: $(BUILD)/cc-host $(CCROOT_READY) $(BUILD)/mlib/libc.a
	python3 tests/nano_build.py
nanotest: nano-build
	python3 tests/nano_smoke.py

.PHONY: nano-sshtest
nano-sshtest: nano-build openssh-tests-ready
	python3 tests/openssh_smoke.py --nano

.PHONY: nano-networktest
nano-networktest: nano-build
	python3 tests/nano_smoke.py --network

# Hardware discovery: real PCI scanner plus disposable legacy/SATA and USB boots.
$(BUILD)/hwinfo-check.o: tests/hwinfo_check.c user/lib/hwinfo.h user/lib/myos.h | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/hwinfo-check: $(BUILD)/hwinfo-check.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
$(BUILD)/user/lspci.o $(BUILD)/user/lsusb.o $(BUILD)/user/lsports.o: user/lib/hwformat.h
.PHONY: hardwaretest
hardwaretest: $(BUILD)/os.img $(HWBUILD)/kernel.bin $(BUILD)/hwinfo-check $(BUILD)/myfs $(BUILD)/ld.so $(BUILD)/user/libc.so $(BUILD)/user/sh $(BUILD)/user/lspci $(BUILD)/user/lsusb $(BUILD)/user/lsports
	gcc -std=gnu11 -DHOST_TEST -fno-builtin -Wall -Wextra -Ikernel tests/pci_inventory_test.c kernel/pci.c -o $(BUILD)/pci-inventory-test
	./$(BUILD)/pci-inventory-test
	$(HOSTCC) -m32 -std=gnu17 -O1 -fno-builtin -ffunction-sections -fdata-sections tests/ahci_inventory_test.c kernel/string.c -Wl,--gc-sections -o $(BUILD)/ahci-inventory-test
	./$(BUILD)/ahci-inventory-test
	python3 tests/hardware_inventory_smoke.py

.PHONY: smpacpitest
smpacpitest: | $(BUILD)
	$(HOSTCC) -std=gnu17 -O1 -g -DHOST_TEST -fno-builtin -fsanitize=address,undefined tests/smp_acpi_test.c kernel/acpi_smp.c kernel/string.c -o $(BUILD)/smp-acpi-test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ./$(BUILD)/smp-acpi-test

$(BUILD)/user/make.o $(BUILD)/user/ext.o: user/lib/build_jobs.h

$(BUILD)/make-job-probe.o: tests/make_job_probe.c user/lib/myos.h | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/make-job-probe: $(BUILD)/make-job-probe.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
.PHONY: parallel-buildtest
parallel-buildtest: $(BUILD)/os.img $(BUILD)/make-job-probe $(BUILD)/myfs $(BUILD)/ld.so $(BUILD)/user/sh $(BUILD)/user/make $(BUILD)/user/ext $(BUILD)/user/cc $(BUILD)/user/tar $(BUILD)/user/cp $(BUILD)/user/ar $(BUILD)/user/echo $(BUILD)/user/libc.so $(BUILD)/user/libgfx.so $(BUILD)/mlib/libc.a $(BUILD)/mlib/libm.a $(BUILD)/mlib/libgfx.a
	python3 tests/parallel_build_smoke.py

.PHONY: usblifecycletest
usblifecycletest: | $(BUILD)
	$(HOSTCC) -m32 -std=gnu17 -O1 -g -fno-builtin -ffunction-sections -fdata-sections tests/usb_lifecycle_test.c kernel/string.c kernel/hid_mouse.c -Wl,--gc-sections -o $(BUILD)/usb-lifecycle-test
	./$(BUILD)/usb-lifecycle-test

$(BUILD)/usb-hotplug-check.o: tests/usb_hotplug_check.c user/lib/myos.h user/lib/hwinfo.h | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@
$(BUILD)/usb-hotplug-check: $(BUILD)/usb-hotplug-check.o $(ULIB) user/user.ld
	$(LD) $(ULDFLAGS) $< $(ULIB) -o $@
.PHONY: usbhotplugtest
usbhotplugtest: $(HWBUILD)/kernel.bin $(BUILD)/usb-hotplug-check $(BUILD)/myfs $(BUILD)/ld.so $(BUILD)/user/sh $(BUILD)/user/lsusb $(BUILD)/user/lsports $(BUILD)/user/libc.so usblifecycletest
	python3 tests/usb_hotplug_smoke.py

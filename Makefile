.PHONY: android-apk windows-cross linux-cross macos-cross

android-apk:
	./mobile/build-android-apk.sh

windows-cross:
	python3 cmake/build-windows.py $(WINDOWS_CROSS_ARGS)

linux-cross:
	python3 cmake/build-unix.py linux $(LINUX_CROSS_ARGS)

macos-cross:
	python3 cmake/build-unix.py macos $(MACOS_CROSS_ARGS)

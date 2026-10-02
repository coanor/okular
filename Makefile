.PHONY: android-apk windows-cross

android-apk:
	./mobile/build-android-apk.sh

windows-cross:
	python3 cmake/build-windows.py $(WINDOWS_CROSS_ARGS)

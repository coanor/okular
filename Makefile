ANDROID_ENV_FILE ?= $(HOME)/.local/share/okular-android/environment.sh

.PHONY: android-apk windows-cross linux-cross macos-cross

android-apk:
	@set -e; \
	if [ -f "$(ANDROID_ENV_FILE)" ]; then . "$(ANDROID_ENV_FILE)"; fi; \
	if [ -S /var/run/docker.sock ] && [ ! -w /var/run/docker.sock ]; then \
		sg docker -c './mobile/build-android-apk.sh'; \
	else \
		./mobile/build-android-apk.sh; \
	fi

windows-cross:
	python3 cmake/build-windows.py $(WINDOWS_CROSS_ARGS)

linux-cross:
	python3 cmake/build-unix.py linux $(LINUX_CROSS_ARGS)

macos-cross:
	python3 cmake/build-unix.py macos $(MACOS_CROSS_ARGS)

ANDROID_ENV_FILE ?= $(HOME)/.local/share/okular-android/environment.sh

.PHONY: android-apk

android-apk:
	@set -e; \
	if [ -f "$(ANDROID_ENV_FILE)" ]; then . "$(ANDROID_ENV_FILE)"; fi; \
	if [ -S /var/run/docker.sock ] && [ ! -w /var/run/docker.sock ]; then \
		sg docker -c './mobile/build-android-apk.sh'; \
	else \
		./mobile/build-android-apk.sh; \
	fi

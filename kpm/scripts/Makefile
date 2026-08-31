ARTIFACT ?= ../stackplz-kpm.kpm
TARGET_COMPILE ?= aarch64-linux-gnu-
PYTHON ?= python3

.PHONY: verify
verify:
	@test -n "$(KP_DIR)" || { echo "KP_DIR is required" >&2; exit 2; }
	ARTIFACT="$(ARTIFACT)" KP_DIR="$(KP_DIR)" \
		TARGET_COMPILE="$(TARGET_COMPILE)" PYTHON="$(PYTHON)" \
		sh ./verify_artifact.sh

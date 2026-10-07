#!/usr/bin/env bash
# Copy Wasabi phone's program to the NAS (no root needed; scp is off on
# the NAS, so tar goes over ssh). After an update, restart the `wasabi`
# container in Portainer (Containers > wasabi > Restart).
set -euo pipefail
cd "$(dirname "$0")/.."
D=/volume2/docker/wasabi
ssh nas "mkdir -p $D/app $D/config/wasabi $D/config/cache $D/files"
tar -cf - wasabi wasabi_view.py wasabi_api.py view/dist | ssh nas "tar -xf - -C $D/app"
# the Amiga's key and the protected volumes: the daemon refuses a client
# without the key. Copied only the first time; kept private.
ssh nas "test -f $D/config/wasabi/config" || {
    ssh nas "umask 077; cat > $D/config/wasabi/config" < "${XDG_CONFIG_HOME:-$HOME/.config}/wasabi/config"
}
# The NAS's shares give every new file to everyone (ACLs); the key and
# the login's password hash must not be. chmod removes those rules.
ssh nas "chmod -R go-rwx $D/config"
ssh nas "cat > /volume2/docker/_portainer-stacks/wasabi.yml" < tools/nas-stack.yml
echo "copied to $D; the stack is /volume2/docker/_portainer-stacks/wasabi.yml"

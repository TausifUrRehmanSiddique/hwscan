# Building the image through git

**Git does not build images.** It is version control — it tracks changes to
text. What it gives you here is the thing that actually matters for a tool that
writes into a shared inventory file: **every image traces back to an exact
commit**, so when a CSV row looks wrong six weeks later you can tell which
build produced it.

The build itself is done by CI. This document sets that up.

---

## Which path do you want?

| You want | Use |
|---|---|
| Version control, build on your own machine | [Part 1](#part-1--put-it-in-git) only |
| Push a tag, get a downloadable image | Part 1 + [Part 2](#part-2--let-ci-build-the-image) |
| GitLab instead of GitHub | Part 1 + [Part 4](#part-4--gitlab-ci) |
| No network, just build it now | Skip this file — see `BUILD-ISO.md` |

CI is worth it once more than one person builds images, or once you need to
answer "which build was that?". For a single operator on one laptop it is
overhead.

---

## Part 1 — Put it in git

### 1.1 Initialise

```bash
cd hwscan-cpp
git init
git add .
git status                      # read this before committing
```

Check `git status` shows source, docs and scripts — **and no `.img`, no
`hwscan` binary, no `build/` or `dist/`**. The shipped `.gitignore` covers
those. If you see a large binary listed, stop and fix `.gitignore` first;
git keeps every version of every file forever, and a 1 GB image committed once
stays in the history permanently even after you delete it.

Git LFS is not the answer here either. LFS is for large files that are *inputs*
to your build. An `.img` is an *output* — rebuild it, don't store it.

```bash
git commit -m "hwscan: hardware inventory scanner, C++ implementation"
```

### 1.2 Set an identity if git asks

```bash
git config --global user.name  "Your Name"
git config --global user.email "you@example.com"
```

### 1.3 Push to a remote

Create an **empty** repository on GitHub (no README, no `.gitignore` — those
would conflict with what you already have), then:

```bash
git remote add origin https://github.com/YOURNAME/hwscan-cpp.git
git branch -M main
git push -u origin main
```

Make it **private** unless you have reviewed what the fixture and docs
contain. Serial numbers, asset tags and MAC addresses are exactly the sort of
thing that ends up in a test file by accident.

### 1.4 Build locally from the clone

Nothing changes — git has not altered the build:

```bash
git clone https://github.com/YOURNAME/hwscan-cpp.git
cd hwscan-cpp
chmod +x iso/build-iso.sh
make test
sudo ./iso/build-iso.sh --docker
```

But you can now stamp the image with the commit it came from:

```bash
HWSCAN_NAME="hwscan-$(git describe --always --dirty)" \
HWSCAN_COMMIT="$(git rev-parse HEAD)" \
sudo -E ./iso/build-iso.sh --docker
```

`-E` matters. Without it `sudo` drops the variables and you silently get the
plain date-stamped name.

The commit lands in `hwscan-build.txt` at the root of the stick, so you can
plug an unknown stick in and ask it what it is.

---

## Part 2 — Let CI build the image

`.github/workflows/build-image.yml` ships with this project. It has two jobs.

**`check`** runs on every push and pull request. About a minute. It compiles
with `-Werror`, builds the static binary, runs the scanner against the
synthetic ThinkPad, asserts the CSV really has 42 columns, and syntax-checks
both `build-iso.sh` (under `bash`) and the embedded `init` (under `dash`, the
closest available proxy for the busybox `ash` that will actually run it).

**`image`** builds the `.img`. It does **not** run on ordinary pushes — a 1 GB
image per commit is a waste of storage quota. It runs when you push a version
tag, or when you click the button.

### 2.1 Why a GitHub runner can do this at all

The build needs root, loop devices, `mkfs.vfat` and `mount`. A GitHub-hosted
runner is a **full VM**, not a container: `sudo` is passwordless, loop devices
work, and Docker is preinstalled with `--privileged` available. The existing
`--docker` path is reused unchanged, so CI builds inside the same
`debian:bookworm` container you would use locally — same kernel package, same
module layout, same result.

### 2.2 Build on demand

1. Push the workflow: `git add .github && git commit -m "ci: build image" && git push`
2. GitHub → **Actions** tab → **Build bootable image** → **Run workflow**
3. Optionally change the size or partition type, then **Run workflow**
4. Wait ~10 minutes for the first run (it pulls the Debian packages)
5. Open the finished run → **Artifacts** → download

The artifact contains:

```
hwscan-YYYYMMDD-<sha>.img.zst      compressed image
hwscan-YYYYMMDD-<sha>.img.sha256   hash of the UNCOMPRESSED image
SHA256SUMS                          hashes of what is in this artifact
```

Artifacts expire after 14 days by default. Change `retention-days` in the
workflow if you need longer.

### 2.3 Build a release

```bash
git tag -a v1.0.0 -m "First bench release"
git push origin v1.0.0
```

That triggers the same build and publishes it as a **GitHub Release**, which
does not expire. Anyone with repo access gets a permanent download URL, and
the release notes carry the flashing commands.

To move a tag you got wrong:

```bash
git tag -d v1.0.0
git push origin :refs/tags/v1.0.0
```

Then re-tag. Be careful doing this once anyone has downloaded the release —
they now hold an image whose version number means something different from
yours, which is precisely the confusion tagging is meant to prevent.

### 2.4 Flash what CI built

```bash
# 1. decompress
zstd -d hwscan-20260824-a1b2c3d.img.zst

# 2. verify against the hash the build recorded
sha256sum -c hwscan-20260824-a1b2c3d.img.sha256

# 3. identify the stick -- confirm SIZE and TRAN=usb before continuing
lsblk -d -o NAME,PATH,SIZE,TRAN,MODEL

# 4. write it
sudo umount /dev/sdX* 2>/dev/null
sudo dd if=hwscan-20260824-a1b2c3d.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

Step 3 is not optional. `dd` to the wrong device destroys it without asking,
and the wrong device is very often the machine's own disk.

---

## Part 3 — Reading the build afterwards

Every image carries `hwscan-build.txt` at the root of its FAT32 partition:

```
name:    hwscan-v1.0.0
built:   2026-08-24T15:02:11Z
kernel:  6.1.0-23-amd64
commit:  a1b2c3d4e5f6...
image:   1024 MB, MBR type 0c
```

Plug an unidentified stick into any machine and read it. To go from that commit
back to the source:

```bash
git show a1b2c3d4
git diff a1b2c3d4 HEAD -- src/
```

That is the entire reason to involve git in this. A stick found in a drawer can
now tell you exactly what code produced the rows it wrote.

---

## Part 4 — GitLab CI

Same idea, different file. Save as `.gitlab-ci.yml`:

```yaml
stages: [check, image]

check:
  stage: check
  image: debian:bookworm
  before_script:
    - apt-get update -qq
    - apt-get install -y --no-install-recommends g++ make python3 dash
  script:
    - make CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror"
    - make clean && make test
    - test "$(head -1 /tmp/hwscan-out/inventory.csv | tr ',' '\n' | wc -l)" -eq 42
    - bash -n iso/build-iso.sh

image:
  stage: image
  image: debian:bookworm
  # Needs a privileged runner. Shared GitLab.com runners are NOT privileged,
  # so this requires your own runner registered with privileged = true.
  tags: [privileged]
  rules:
    - if: $CI_COMMIT_TAG
    - if: $CI_PIPELINE_SOURCE == "web"
  variables:
    HWSCAN_NAME: "hwscan-${CI_COMMIT_SHORT_SHA}"
    HWSCAN_COMMIT: "${CI_COMMIT_SHA}"
  before_script:
    # --native means this job installs the toolchain itself. grub2-common is
    # the one that ships grub-install; grub-common does not.
    - apt-get update -qq
    - |
      apt-get install -y --no-install-recommends \
        g++ make xorriso grub-pc-bin grub-efi-amd64-bin grub-common grub2-common \
        mtools dosfstools cpio gzip xz-utils zstd kmod pciutils \
        busybox-static linux-image-amd64 ca-certificates fdisk util-linux parted
  script:
    - chmod +x iso/build-iso.sh
    - ./iso/build-iso.sh --native
    - cd dist && zstd -19 -T0 --rm -f *.img && sha256sum ./* > SHA256SUMS
  artifacts:
    paths: [dist/]
    expire_in: 2 weeks
```

**The privileged-runner requirement is the catch.** GitLab.com's shared runners
cannot create loop devices, so the image job will fail on them. You need a
self-hosted runner registered with `privileged = true` in `config.toml`. If
that is not available, build locally and attach the image to a GitLab Release
by hand.

Note this uses `--native` rather than `--docker`, because the job is already
running inside `debian:bookworm` — nesting Docker inside CI is avoidable
complexity.

---

## What CI does not solve

* **Secure Boot.** CI cannot sign the image. `grub-install --removable`
  produces an unsigned `BOOTX64.EFI`, and signing needs shim plus a
  Microsoft-signed chain. Secure Boot stays disabled on the machines you scan.
* **Hardware testing.** The `check` job proves the scanner compiles and handles
  a synthetic machine. It cannot tell you whether SMART works on a real SATA
  drive, whether EDID parses on a real panel, or whether the image boots on a
  particular firmware. Those need a bench.
* **The image builder itself.** The `image` job runs the real build on real
  loop devices, so it does exercise `build_img` — which is more than can be
  said for any environment it was written in. But "the build succeeded" and
  "the stick boots that HP" are different claims.

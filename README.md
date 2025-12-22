<!--
SPDX-FileCopyrightText: 2025 Dominik Wombacher <dominik@wombacher.cc>

SPDX-License-Identifier: CC0-1.0
-->

# AWS Neuron Driver

## Disclaimer

**This is a personal project and not related to,
or endorsed by, Amazon Web Services.**

-----

This repository contains all available versions of the AWS Neuron Driver
source code (`GPL-2.0-only` license). A Linux kernel device driver
supporting the AWS Neuron SDK.

There is **no** independent development of features or bug fixes done in this repository.
It is basically a mirror of the source code as it is released by AWS.

[![REUSE status](https://api.reuse.software/badge/git.sr.ht/~wombelix/aws-neuron-driver)](https://api.reuse.software/info/git.sr.ht/~wombelix/aws-neuron-driver)

## Table of Contents

* [Why?](#why)
* [Why continue?](#why-continue)
* [How?](#how)
* [When?](#when)
* [Usage](#usage)
* [Source](#source)
* [Contribute](#contribute)
* [License](#license)

## Why

The [official repository](https://github.com/aws-neuron/aws-neuron-driver)
had no new commit since
[October 2020](https://github.com/aws-neuron/aws-neuron-driver/commit/cad466aa978944bc8ef51f2228c2ca80618d3103).
The driver, which is open source under the `GPL-2.0-only` license, was only
distributed as rpm package with a DKMS wrapper. And not available as archive
or in a public git repository.

In February 2025 I started to publish the code in this repository.
1/ for consumption in a project I worked on at that time and
2/ it felt like the right thing to do from an open source community perspective.

Since end of September 2025, AWS published the version
[2.24.7.0](https://github.com/aws-neuron/aws-neuron-driver/commit/7c0d02c11a34ac464c785d584e8289ae00be0adc)
and
[2.25.4.0](https://github.com/aws-neuron/aws-neuron-driver/commit/ec131bf4749024b05470f6bb72f5273366b2f2c7)
source code their official repository. And it looks like
they continue to do that with upcoming releases too.

So, either a coincidence or someone saw what I was doing and liked it.
Either way, It's great that AWS continued to publish the code!

For now, I decided to continue publishing the code through my own process.

## Why continue

There are slight differences the in the way AWS publishes the code and
[how](#how) I do it.

It looks like that AWS has a bot running externally that triggers a new Pull Request
And this PR gets auto-approved by a
[GitHub Action workflow](https://github.com/aws-neuron/aws-neuron-driver/blob/master/.github/workflows/enable_automerge.yml).
There isn't visibility where the code is exactly coming from.

At this point my approach provides validation mechanisms
and more transparency. That's why see value in continuing with this repo.

## How

The tool [aws-neuron-driver-publish-source](https://git.sr.ht/~wombelix/aws-neuron-driver-publish-source)
is used to add releases and code updates to this unofficial repository.
Checksum and GPG verifications are performed and metadata added.
This creates an audit trail and allows independent validation
that the code is coming from the official repository
[yum.repos.neuron.amazonaws.com](https://yum.repos.neuron.amazonaws.com/)
and wasn't altered.

### Folder structure

```
src/            =   AWS Neuron Driver source code
archive/        =   Public GPG Key and repo xml files
        rpm/    =   RPM files with SHA256 checksums
```

The `archive/` folder contains copies of various files processed while
adding a new Neuron Driver source code version to the repository.
They can be used to verify and validate the authenticity of the code in `src/`.

### Example commit message

Every commit of a new Driver version contains additional Metadata. If available,
[Release Notes](https://awsdocs-neuron.readthedocs-hosted.com/en/latest/release-notes/runtime/aws-neuronx-dkms/index.html)
are added as well. These commits are also tagged with the version number.
An example commit during
[aws-neuron-driver-publish-source](https://git.sr.ht/~wombelix/aws-neuron-driver-publish-source)
development:

```
commit 053a860d1eb7fbeb68aa1e887eb4368ddece27f6 (tag: 1.1)
Author: Dominik Wombacher <dominik@wombacher.cc>
Date:   Wed Feb 5 08:48:09 PM UTC 2025 +0000

    feat: Neuron Driver 1.1

    Source code extracted from file: aws-neuron-dkms-1.1-2.0.noarch.rpm
    Downloaded from repository: https://yum.repos.neuron.amazonaws.com


    Metadata
    --------
    Package: aws-neuron-dkms
    Version: 1.1
    License: Unknown
    Summary: aws-neuron 1.1 dkms package
    Description: Kernel modules for aws-neuron 1.1 in a DKMS wrapper.
    Filename: aws-neuron-dkms-1.1-2.0.noarch.rpm
    Checksum: d994cd63745e7306bf9583c74468a40f46e199d698e2fd28610209483c311d6a
    Buildhost: 0cbde921ec7e
    Buildtime: 2020-10-19 18:45:40 +0000 UTC
    GPG key primary uid: Amazon AWS Neuron <neuron-maintainers@amazon.com>
    GPG key creation time: 2019-11-11 17:29:27 +0000 UTC
    GPG key fingerprint: 00FA2C1079260870A76D2C285749CAD8646D9185
    GPG check: OK
    SHA256 check: OK
    --------


    Files
    -----
    - M: src/README.md
    - M: src/postinstall
    - ?: archive/rpm/aws-neuron-dkms-1.1-2.0.noarch.rpm
    - ?: archive/rpm/aws-neuron-dkms-1.1-2.0.noarch.rpm.sha256
    - M: src/aws-neuron-dkms-mkrpm.spec
    - M: src/postremove
    - M: src/aws-neuron-dkms-mkdeb/debian/rules
    - M: src/dkms.conf
    - M: src/neuron_dma.c
    - M: src/neuron_module.c
    - M: src/aws-neuron-dkms-mkdeb/debian/postinst
    - M: src/aws-neuron-dkms-mkdeb/debian/prerm
    - D: src/LICENSE
    - M: src/preinstall
    -----
```

## When

Source code updates are published automatically through two mechanisms:

### GitHub Actions Workflow

A [GitHub Actions workflow](.github/workflows/check-neuron-driver.yml)
runs every Sunday at 3:17 AM UTC and can be triggered manually. The
workflow compares the latest AWS Neuron Driver release notes with
the local copy. If changes are detected, it triggers an sr.ht build
to publish new source code.

### sr.ht Build Manifest

The [sr.ht build manifest](.build.yaml) contains a `publish` task that
downloads and compares release notes from the official AWS repository.
It only proceeds if new releases are detected. The task builds and runs
the [aws-neuron-driver-publish-source](https://git.sr.ht/~wombelix/aws-neuron-driver-publish-source)
tool, commits and pushes new driver source code with metadata. It uses
the `skip-ci` option to prevent infinite build loops.

This ensures the repository stays current with official AWS releases automatically.

## Usage

Please refer to [src/README.md](src/README.md) for technical details about the driver,
as well as usage and build instructions.

## Source

The primary location is:
[git.sr.ht/~wombelix/aws-neuron-driver](https://git.sr.ht/~wombelix/aws-neuron-driver)

Mirrors are available on
[Codeberg](https://codeberg.org/wombelix/aws-neuron-driver),
[Gitlab](https://gitlab.com/wombelix/aws-neuron-driver)
and
[GitHub](https://github.com/wombelix/aws-neuron-driver).

## Contribute

I don't intend to make changes to the `aws-neuron-driver` source
in any way in this repository. So, contributions are limited to
content outside the `src/` and `archive/` sub-folders.

With this limitation in mind, please don't hesitate to provide Feedback,
open an Issue or create a Pull / Merge Request.

Just pick the workflow or platform you prefer and are most comfortable with.

Feedback, bug reports or patches to my sr.ht list
[~wombelix/inbox@lists.sr.ht](https://lists.sr.ht/~wombelix/inbox) or via
[Email and Instant Messaging](https://dominik.wombacher.cc/pages/contact.html)
are also always welcome.

## License

Unless otherwise stated:

* `aws-neuron-driver` code in the `src/` and `archive/rpm/` sub-folder: `GPL-2.0-only`
* Computer generated files in sub-folder `archive/`: `MIT-0`
* [Neuron Driver Release Notes](archive/release-notes-runtime-aws-neuronx-dkms.rst)
  in `archive/`: `CC-BY-SA-4.0`
* Files in the repository `root folder`: `CC0-1.0`

All files contain license information either as
`header comment`, `corresponding .license` file or have annotation
instructions defined in the `REUSE.toml` config.

[REUSE](https://reuse.software) from the [FSFE](https://fsfe.org/)
implemented to verify license and copyright compliance.

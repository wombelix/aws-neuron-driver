<!--
SPDX-FileCopyrightText: 2025 Dominik Wombacher <dominik@wombacher.cc>

SPDX-License-Identifier: CC0-1.0
-->

# AWS Neuron Driver

## Disclaimer

**This is a personal project and not related to,
or endorsed by, Amazon Web Services.**

-----

This repository contains all available versions of the `GPL-2.0-only`
source code of the AWS Neuron Driver. A Linux kernel device driver
supporting the AWS Neuron SDK.

[![REUSE status](https://api.reuse.software/badge/git.sr.ht/~wombelix/aws-neuron-driver)](https://api.reuse.software/info/git.sr.ht/~wombelix/aws-neuron-driver)

## Table of Contents

* [Why?](#why)
* [How?](#how)
* [Usage](#usage)
* [Source](#source)
* [Contribute](#contribute)
* [License](#license)

## Why

The [official repository](https://github.com/aws-neuron/aws-neuron-driver)
doesn't contain recent versions of the driver source code. It is still under
`GPL-2.0-only` but only distributed as rpm package with a DKMS wrapper.
Releases since October 2020 are not available as archive or in a public git repository.

## How

The tool [aws-neuron-driver-publish-source](https://git.sr.ht/~wombelix/aws-neuron-driver-publish-source)
is used to add releases and code updates to this unofficial repository.
Checksum and GPG verifications are performed and metadata added.
This creates an audit trail and allows to validate that the code is coming
from the official repository
[yum.repos.neuron.amazonaws.com](https://yum.repos.neuron.amazonaws.com/)
and wasn't altered.

There is no independent development done in this repository.
It is basically a mirror of the source code as it is released by AWS.

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

I don't intend to alter the `aws-neuron-driver` source in any way in this repository.
So, contributions are limited to content outside the `src/` and `archive/` sub-folders.

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
* Computer generated files in sub-folder `archive`: `MIT-0`
* Files in the repository root folder: `CC0-1.0`

All files contain license information either as
`header comment`, `corresponding .license` file or have annotation
instructions defined in the `REUSE.toml` config.

[REUSE](https://reuse.software) from the [FSFE](https://fsfe.org/)
implemented to verify license and copyright compliance.

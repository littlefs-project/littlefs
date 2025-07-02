# Contributing to little fs

We would love for you to contribute to Arm mBED Little FS and help make it even better than it is today! As a contributor, here are the guidelines we would like you to follow.

- [Contributing to little fs](#contributing-to-little-fs)
  - [Code of Conduct](#code-of-conduct)
  - [Bugs](#bugs)
  - [New Features](#new-features)
  - [Contributing Code](#contributing-code)
    - [Initial Setup](#initial-setup)
    - [Contributor License Agreement (CLA)](#contributor-license-agreement-cla)
    - [Submitting a Pull Request](#submitting-a-pull-request)
      - [Branch](#branch)
      - [Create Commits](#create-commits)
      - [Upstream Sync and Clean Up](#upstream-sync-and-clean-up)
      - [Coding Conventions and Style](#coding-conventions-and-style)
      - [Submit Pull Request](#submit-pull-request)

## Code of Conduct

TBD

## Bugs

If you find a bug in the source code, you can help us by [submitting a GitHub Issue](https://github.com/ARMmbed/littlefs/issues/new).  The best bug reports provide a detailed description of the issue and step-by-step instructions for predictably reproducing the issue.  Even better, you can [submit a Pull Request](#submitting-a-pull-request) with a fix.

## New Features

You can request a new feature by [submitting a GitHub Issue](https://github.com/ARMmbed/littlefs/issues/new).

If you would like to implement a new feature, please consider the scope of the new feature:

* *Large feature*: first [submit a GitHub Issue](https://github.com/ARMmbed/littlefs/issues/new) and communicate your proposal so that the community can review and provide feedback.  Getting early feedback will help ensure your implementation work is accepted by the community.  This will also allow us to better coordinate our efforts and minimize duplicated effort.

* *Small feature*: can be implemented and directly [submitted as a Pull Request](#submitting-a-pull-request).

## Contributing Code

The Little FS Project follows the "Fork-and-Pull" model for accepting contributions.

### Initial Setup

Setup your GitHub fork and continuous-integration services:

1. Fork the [Little FS repository](https://github.com/ARMmbed/littlefs) by clicking "Fork" on the web UI.

Setup your local development environment:

```bash
# Clone your fork
git clone https://github.com/<user name>/littlefs.git

# Configure upstream alias
git remote add upstream https://github.com/ARMmbed/littlefs.git
```

### Contributor License Agreement (CLA)

[License](LICENSE.md)

### Submitting a Pull Request

#### Branch

For each new feature, create a working branch:

```bash
# Checkout the branch '-b' creates a new branch
git checkout -b <branch-name>
```

#### Create Commits

```bash
# Add each modified file you'd like to include in the commit
git add <file1> <file2>

# Create a commit
git commit
```

This will open up a text editor where you can craft your commit message.

#### Upstream Sync and Clean Up

Prior to submitting your pull request, you might want to do a few things to clean up your branch and make it as simple as possible for the original repo's maintainer to test, accept, and merge your work.

If any commits have been made to the upstream master branch, you should rebase your development branch so that merging it will be a simple fast-forward that won't require any conflict resolution work.

```bash
# Fetch upstream master and merge with your repo's master branch
git checkout master
git pull upstream master

# If there were any new commits, rebase your development branch
git checkout <branch-name>
git rebase master
```

Now, it may be desirable to squash some of your smaller commits down into a small number of larger more cohesive commits. You can do this with an interactive rebase:

```bash
# Rebase all commits on your development branch
git checkout
git rebase -i master
```

This will open up a text editor where you can specify which commits to squash.

#### Coding Conventions and Style

TBD

#### Submit Pull Request

Go to the page for your fork on GitHub, select your development branch, and click the pull request button. If you need to make any adjustments to your pull request, just push the updates to GitHub. Your pull request will automatically track the changes on your development branch and update.
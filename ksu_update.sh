#!/bin/bash

echo pulling main branch of KernelSU

git subtree pull --prefix=drivers/staging/kernelsu https://github.com/rifsxd/KernelSU-Next.git next --squash -m "drivers: kernelsu: update"

echo done

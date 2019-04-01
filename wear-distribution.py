#!/usr/bin/env python

import matplotlib
matplotlib.use('SVG')
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec
import numpy as np

matplotlib.rc('font', family='sans-serif', size=11)
matplotlib.rc('axes', titlesize='medium', labelsize='medium')
matplotlib.rc('xtick', labelsize='small')
matplotlib.rc('ytick', labelsize='small')

np.random.seed(map(ord, "hello"))

gs = gridspec.GridSpec(nrows=2, ncols=2, height_ratios=[1, 2],
        wspace=0.25, hspace=0.25)

fig = plt.figure(figsize=(7, 5.5))
ax1 = fig.add_subplot(gs[0, 0])
ax2 = fig.add_subplot(gs[0, 1], sharex=ax1, sharey=ax1)
ax3 = fig.add_subplot(gs[1, :])

# Convolution (I think?) of two types of uniform distributions under modulo
w1 = np.random.randint(0, 100, 30)
w2 = np.arange(0, 30, 1)
w3 = np.array([w2 + i for i in w1]).flatten() % 100

ax1.hist(w1, bins=100, range=(0,100), histtype='stepfilled')
ax2.hist(w2, bins=100, range=(0,100), histtype='stepfilled')
ax3.hist(w3, bins=100, range=(0,100), histtype='stepfilled')

ax1.set_title('Random offset')
ax2.set_title('Linear allocation')
ax3.set_title('Cumulative allocation')

ax1.set_ylabel('wear')
ax1.set_yticks(np.arange(0, 3))
ax3.set_ylabel('wear')
ax3.set_xlabel('block address')

for ax in [ax1,ax2,ax3]:
    ax.set_xlim(0, 100)
    ax.set_xticklabels([])
    ax.set_yticklabels([])
    ax.spines['right'].set_visible(False)
    ax.spines['top'].set_visible(False)

ax1.text(109.25, 1, '$\\ast$')

fig.tight_layout()
plt.savefig('wear-distribution.svg', bbox_inches="tight")

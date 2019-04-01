#!/usr/bin/env python

import matplotlib
matplotlib.use('SVG')
import matplotlib.pyplot as plt
import numpy as np

matplotlib.rc('font', family='sans-serif', size=11)
matplotlib.rc('axes', titlesize='medium', labelsize='medium')
matplotlib.rc('xtick', labelsize='small')
matplotlib.rc('ytick', labelsize='small')

fig, ax = plt.subplots(figsize=(6, 3.5))
ax.set_title('Metadata pair update cost')

# multiplicative cost of metadata
def cost(r, n, size):
    cost = n + n*(r*(size/n) / ((1-r)*(size/n) + 1))
    return cost / n

t = np.arange(0.0, 1.0, 0.01)
ax.plot(t, cost(t, 100, 4096))

ax.set_xlabel('% full')
ax.set_xlim(0, 1)
ax.set_ylabel('multiplicative cost')
ax.set_ylim(0, 12)

xticks = np.arange(0.0, 1+0.1, 0.2)
ax.set_xticks(xticks)
ax.set_xticklabels(['%d%%' % (100*t) for t in xticks])
yticks = np.arange(0, 12+1, 2)
ax.set_yticks(yticks)
ax.set_yticklabels(['%dx' % t for t in yticks])

ax.spines['right'].set_visible(False)
ax.spines['top'].set_visible(False)

fig.tight_layout()
plt.savefig('metadata-cost.svg', bbox_inches="tight")

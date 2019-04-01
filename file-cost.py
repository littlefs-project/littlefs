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
ax.set_title('File storage cost')

# multiplicative cost of inline files
def inline_cost(n, B):
    #return 4+n-n
    return np.full_like(n, 4)

# multiplicative cost of ctz skip-list files
def ctz_cost(n, B):
    return (np.ceil(n/B)*B) / n

# combined multiplicative cost
def file_cost(n, B):
    return np.where(n < B/4, inline_cost(n, B), ctz_cost(n, B))

t = np.arange(0.0, 80.0, 0.25)
ax.plot(t, file_cost(t, 16))
t1 = np.arange(0.0, 16.0/4.0 + 0.5, 0.25)
t2 = np.arange(16.0/4.0, 80.0, 0.25)
ax.plot(t1, ctz_cost(t1, 16), color='C0', linestyle=(2.65, (3.65,1.65)))
ax.plot(t2, inline_cost(t2, 16), color='C0', linestyle=(3.65, (3.65,1.65)))

ax.text(70.5, 4.15, 'inline file', size='small')
ax.text(3.3, 5.85, 'ctz skip-list', rotation=-85, size='small')

ax.set_xlabel('file size')
ax.set_xlim(0, 80)
ax.set_ylabel('multiplicative cost')
ax.set_ylim(0, 6)

xticks = np.arange(0, 80+1, 16)
ax.set_xticks(xticks)
ax.set_xticklabels(['%dKiB' % ((t*(4096/16))/1024) for t in xticks])
yticks = np.arange(0, 6+1, 2)
ax.set_yticks(yticks)
ax.set_yticklabels(['%dx' % t for t in yticks])

ax.spines['right'].set_visible(False)
ax.spines['top'].set_visible(False)

fig.tight_layout()
plt.savefig('file-cost.svg', bbox_inches="tight")

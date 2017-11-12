#!/usr/bin/env python

import random

from utils.tools import Rate
from ._generator import gen_sweep_async
from .colors import Colors


class SweepAsync(Colors):

    def __init__(self):
        self.PARAMS_LIST['uapp'] = ['swipe']
        Colors.__init__(self, gen_sweep_async)


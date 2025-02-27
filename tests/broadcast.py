# Copyright (c) Chris Choy (chrischoy@ai.stanford.edu).
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
# of the Software, and to permit persons to whom the Software is furnished to do
# so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#
# Please cite "4D Spatio-Temporal ConvNets: Minkowski Convolutional Neural
# Networks", CVPR'19 (https://arxiv.org/abs/1904.08755) if you use any part
# of the code.
import torch
import unittest

from MinkowskiEngine import SparseTensor, MinkowskiGlobalPooling, \
    MinkowskiBroadcastFunction, MinkowskiBroadcastAddition, \
    MinkowskiBroadcastMultiplication, OperationType

from utils.gradcheck import gradcheck
from tests.common import data_loader


class TestBroadcast(unittest.TestCase):

    def test_broadcast_gpu(self):
        in_channels, D = 2, 2
        coords, feats, labels = data_loader(in_channels)
        coords, feats_glob, labels = data_loader(in_channels)
        feats = feats.double()
        feats_glob = feats_glob.double()
        input = SparseTensor(feats, coords=coords)
        pool = MinkowskiGlobalPooling(dimension=D)
        input_glob = pool(input)
        input_glob.F.requires_grad_()
        broadcast = MinkowskiBroadcastAddition(D)
        output = broadcast(input, input_glob)
        print(output)

        # Check backward
        fn = MinkowskiBroadcastFunction()

        device = torch.device('cuda')
        input = input.to(device)
        input_glob = input_glob.to(device)
        output = broadcast(input, input_glob)
        print(output)
        self.assertTrue(
            gradcheck(
                fn,
                (input.F, input_glob.F, OperationType.ADDITION,
                 input.coords_key, input_glob.coords_key, input.coords_man)))

        self.assertTrue(
            gradcheck(
                fn,
                (input.F, input_glob.F, OperationType.MULTIPLICATION,
                 input.coords_key, input_glob.coords_key, input.coords_man)))

    def test_broadcast(self):
        in_channels, D = 2, 2
        coords, feats, labels = data_loader(in_channels)
        coords, feats_glob, labels = data_loader(in_channels)
        feats = feats.double()
        feats_glob = feats_glob.double()
        input = SparseTensor(feats, coords=coords)
        pool = MinkowskiGlobalPooling(dimension=D)
        input_glob = pool(input)
        input_glob.F.requires_grad_()
        broadcast = MinkowskiBroadcastAddition(D)
        broadcast_mul = MinkowskiBroadcastMultiplication(D)
        output = broadcast(input, input_glob)
        print(output)
        output = broadcast_mul(input, input_glob)
        print(output)

        # Check backward
        fn = MinkowskiBroadcastFunction()

        self.assertTrue(
            gradcheck(
                fn,
                (input.F, input_glob.F, OperationType.ADDITION,
                 input.coords_key, input_glob.coords_key, input.coords_man)))

        self.assertTrue(
            gradcheck(
                fn,
                (input.F, input_glob.F, OperationType.MULTIPLICATION,
                 input.coords_key, input_glob.coords_key, input.coords_man)))


if __name__ == '__main__':
    unittest.main()

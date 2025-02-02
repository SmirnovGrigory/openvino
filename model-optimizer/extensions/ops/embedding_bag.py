# Copyright (C) 2018-2021 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

import numpy as np

from mo.graph.graph import Node, Graph
from mo.ops.op import Op


class EmbeddingBagBase(Op):
    enabled = False

    op = op_type = None
    version = None
    in_ports_count = None

    def __init__(self, graph: Graph, attrs: dict):
        super().__init__(graph, {
            'op': self.op,
            'type': self.op_type,
            'version': self.version,

            'infer': self.infer,

            'in_ports_count': self.in_ports_count,
            'out_ports_count': 1,
        }, attrs)

    @staticmethod
    def infer(node: Node):
        raise NotImplementedError('Please use specialized EmbeddingBag operation class, EmbeddingBagBase is base class')


class EmbeddingBagOffsetsSum(EmbeddingBagBase):
    op = op_type = 'EmbeddingBagOffsetsSum'
    version = 'opset3'
    in_ports_count = 5

    @staticmethod
    def infer(node: Node):
        name = node.soft_get('name', node.id)

        connected_in_ports = {idx: port for idx, port in node.in_ports().items() if not port.disconnected()}
        assert len(connected_in_ports) >= 3 and all(p in connected_in_ports for p in [0, 1, 2]), \
            "EmbeddingBagOffsetsSum should have at least 3 connected input port, but it doesn't " \
            "for node: `{}`. Ports: {}".format(name, connected_in_ports)

        weights_shape = node.in_port(0).data.get_shape()
        assert len(weights_shape) >= 2,\
            "EmbeddingBagOffsetsSum should have at least 2D weights for node: `{}`".format(name)
        offsets_shape = node.in_port(2).data.get_shape()
        assert offsets_shape is not None and len(offsets_shape) == 1,\
            "Rank of the offsets in EmbeddingBagOffsetsSum should be equal to 1 for node: `{}`".format(name)

        node.out_port(0).data.set_shape(np.ma.concatenate((offsets_shape[:1], weights_shape[1:])))


class EmbeddingBagPackedSum(EmbeddingBagBase):
    op = op_type = 'EmbeddingBagPackedSum'
    version = 'opset3'
    in_ports_count = 3

    @staticmethod
    def infer(node: Node):
        name = node.soft_get('name', node.id)

        connected_in_ports = {idx: port for idx, port in node.in_ports().items() if not port.disconnected()}
        assert len(connected_in_ports) >= 2 and all(p in connected_in_ports for p in [0, 1]), \
            "EmbeddingBagPackedSum should have at least 2 connected input port, but it doesn't for node: `{}`. " \
            "Ports: {}".format(name, connected_in_ports)

        weights_shape = node.in_port(0).data.get_shape()
        assert len(weights_shape) >= 2, \
            "EmbeddingBagPackedSum should have at least 2D weights for node: `{}`".format(name)
        input_shape = node.in_port(1).data.get_shape()

        node.out_port(0).data.set_shape(np.ma.concatenate((input_shape[:1], weights_shape[1:])))


class EmbeddingSegmentsSum(EmbeddingBagBase):
    op = op_type = 'EmbeddingSegmentsSum'
    version = 'opset3'
    in_ports_count = 6

    @staticmethod
    def infer(node: Node):
        name = node.soft_get('name', node.id)

        connected_in_ports = {idx: port for idx, port in node.in_ports().items() if not port.disconnected()}
        assert len(connected_in_ports) >= 4 and all(p in connected_in_ports for p in [0, 1, 2, 3]), \
            "EmbeddingSegmentsSum should have at least 4 connected input port, but it doesn't for node: `{}`. " \
            "Ports: {}".format(name, connected_in_ports)

        weights_shape = node.in_port(0).data.get_shape()
        assert len(weights_shape) >= 2,\
            "EmbeddingSegmentsSum should have at least 2D weights for node: `{}`".format(name)
        indices_shape = node.in_port(1).data.get_shape()
        segment_ids = node.in_port(2).data.get_shape()
        assert len(indices_shape) == 1 and len(segment_ids) == 1 and indices_shape == segment_ids,\
            "Both indices and segment_ids should have the same shape for node: `{}`".format(name)
        num_segments = node.in_port(3).data.get_value()
        assert num_segments is not None, "EmbeddingSegmentsSum should have a constant num_segments provided, but it " \
                                         "doesn't for node: `{}`.".format(name)
        output_shape = np.ma.concatenate(([num_segments], weights_shape[1:]))
        node.out_port(0).data.set_shape(output_shape)

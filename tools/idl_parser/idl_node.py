#!/usr/bin/env python3
# Copyright 2012 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
# Source: chromium/chromium tag 132.0.6834.83, tools/idl_parser/idl_node.py.

import sys


def CopyToList(item):
    if not item:
        item = []
    if type(item) is not list:
        item = [item]
    return list(item)


class IDLSearch(object):
    def __init__(self):
        self.depth = 0

    def Enter(self, node):
        pass

    def Exit(self, node):
        pass


class IDLAttribute(object):
    def __init__(self, name, value):
        self._cls = 'Property'
        self.name = name
        self.value = value

    def __str__(self):
        return '%s=%s' % (self.name, self.value)

    def GetClass(self):
        return self._cls


class IDLNode(object):
    VERBOSE_PROPS = [
        'PROD', 'NAME', 'VALUE', 'TYPE', 'ERRORS', 'WARNINGS', 'FILENAME',
        'LINENO', 'POSITION',
    ]

    def __init__(self, cls, filename, lineno, pos, children=None):
        self._cls = cls
        self._properties = {
            'ERRORS': [],
            'WARNINGS': [],
            'FILENAME': filename,
            'LINENO': lineno,
            'POSITION': pos,
        }
        self._children = []
        self._parent = None
        self.AddChildren(children)

    def __str__(self):
        name = self.GetProperty('NAME', '')
        value = self.GetProperty('VALUE')
        if value or value == '':
            return '%s(%s) = "%s"' % (self._cls, name, value)
        return '%s(%s)' % (self._cls, name)

    def GetLogLine(self, msg):
        filename, lineno = self.GetFileAndLine()
        return '%s(%d) : %s\n' % (filename, lineno, msg)

    def Error(self, msg):
        self.GetProperty('ERRORS').append(msg)
        sys.stderr.write(self.GetLogLine('error: ' + msg))

    def Warning(self, msg):
        self.GetProperty('WARNINGS').append(msg)
        sys.stdout.write(self.GetLogLine('warning:' + msg))

    def GetFileAndLine(self):
        return self.GetProperty('FILENAME'), self.GetProperty('LINENO')

    def GetClass(self):
        return self._cls

    def GetName(self):
        return self.GetProperty('NAME')

    def GetParent(self):
        return self._parent

    def Traverse(self, search, filter_nodes):
        if self._cls in filter_nodes:
            return ''
        search.Enter(self)
        search.depth += 1
        for child in self._children:
            child.Traverse(search, filter_nodes)
        search.depth -= 1
        search.Exit(self)

    def Tree(self, filter_nodes=None, suppress_props=VERBOSE_PROPS):
        class DumpTreeSearch(IDLSearch):
            def __init__(self, props):
                IDLSearch.__init__(self)
                self.out = []
                self.props = props or []

            def Enter(self, node):
                tab = ''.rjust(self.depth * 2)
                self.out.append(tab + str(node))
                proplist = []
                for key, value in node.GetProperties().items():
                    if key not in self.props:
                        proplist.append(tab + '  %s: %s' % (key, str(value)))
                if proplist:
                    self.out.extend(proplist)

        if filter_nodes is None:
            filter_nodes = ['SpecialComment']
        search = DumpTreeSearch(suppress_props)
        self.Traverse(search, filter_nodes)
        return search.out

    def IsA(self, *typelist):
        return self._cls in typelist

    def GetChildren(self):
        return self._children

    def GetListOf(self, *keys):
        return [child for child in self.GetChildren() if child.GetClass() in keys]

    def GetOneOf(self, *keys):
        out = self.GetListOf(*keys)
        return out[0] if out else None

    def AddChildren(self, children):
        for child in CopyToList(children):
            if not child:
                continue
            if type(child) is IDLAttribute:
                self.SetProperty(child.name, child.value)
            elif type(child) is IDLNode:
                child._parent = self
                self._children.append(child)
            else:
                raise RuntimeError('Adding child of type %s.\n' % type(child).__name__)

    def SetProperty(self, name, val):
        self._properties[name] = val

    def GetProperty(self, name, default=None):
        return self._properties.get(name, default)

    def GetProperties(self):
        return self._properties

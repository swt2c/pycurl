#! /usr/bin/env python
# -*- coding: utf-8 -*-
# vi:ts=4:et

from . import localhost
import asyncio
import pycurl
import pytest
import sys
import unittest

from . import appmanager
from . import util

setup_module, teardown_module = appmanager.setup(('app', 8380))

class MultiCallbackTest(unittest.TestCase):
    def setUp(self):
        self.easy = util.DefaultCurl()
        self.easy.setopt(pycurl.URL, 'http://%s:8380/long_pause' % localhost)
        self.multi = pycurl.CurlMulti()
        self.multi.setopt(pycurl.M_SOCKETFUNCTION, self.socket_callback)
        self.multi.setopt(pycurl.M_TIMERFUNCTION, self.timer_callback)
        self.socket_result = None
        self.timer_result = None
        self.handle_added = False
        self.loop = asyncio.new_event_loop()
        self.timer_handle = None

    def tearDown(self):
        if self.timer_handle:
            self.timer_handle.cancel()
            self.timer_handle = None
        if self.loop.is_running:
            self.loop.stop()
        if not self.loop.is_closed():
            self.loop.close()
        if self.handle_added:
            self.multi.remove_handle(self.easy)
        self.multi.close()
        self.easy.close()

    def socket_callback(self, ev_bitmask, sock_fd, multi, data):
        print("BLAH socket_cb", ev_bitmask, sock_fd)
        self.socket_result = (sock_fd, ev_bitmask)
        if ev_bitmask & pycurl.POLL_REMOVE:
            self.loop.remove_reader(sock_fd)
            self.loop.remove_writer(sock_fd)
        else:
            if ev_bitmask & pycurl.POLL_IN:
                print("BLAH ADDING READER")
                self.loop.add_reader(
                    sock_fd,
                    self.socket_ready_callback,
                    sock_fd,
                    pycurl.CSELECT_IN,
                )
            if ev_bitmask & pycurl.POLL_OUT:
                print("BLAH ADDING WRITER")
                self.loop.add_writer(
                    sock_fd,
                    self.socket_ready_callback,
                    sock_fd,
                    pycurl.CSELECT_OUT,
                )

    def socket_ready_callback(self, sock_fd, ev_bitmask):
        #print("BLAH socket ready", sock_fd, ev_bitmask)
        self.multi.socket_action(sock_fd, ev_bitmask)

    def timer_callback(self, timeout_ms):
        print("BLAH timer_cb", timeout_ms)
        self.timer_result = timeout_ms
        if self.timer_handle:
            self.timer_handle.cancel()
            self.timer_handle = None
        if timeout_ms < 0:
            return
        if timeout_ms == 0:
            timeout_ms = 1
        self.timer_handle = self.loop.call_later(
            timeout_ms / 1000,
            self.timer_expired_callback,
        )

    def timer_expired_callback(self):
        print("BLAH timer_callback_expired!!!")
        self.timer_handle = None
        self.multi.socket_action(pycurl.SOCKET_TIMEOUT, 0)

    def partial_transfer(self):
        def write_callback(data):
            print("BLAH write_callback")
            self.loop.stop()
        self.easy.setopt(pycurl.WRITEFUNCTION, write_callback)
        self.multi.add_handle(self.easy)
        self.handle_added = True
        print("BLAH1")
        self.multi.socket_action(pycurl.SOCKET_TIMEOUT, 0)
        print("BLAH2")
        # Escape valve that will end the loop if nothing happens for 60s
        def stop_loop():
            self.loop.stop()
            raise Exception('Test timed out after 60 seconds')
        self.loop.call_later(60, stop_loop)
        self.loop.run_forever()
        print("BLAH3")
        #while self.sockets and perform:
            #for socket, action in tuple(self.sockets.items()):
            #    self.multi.socket_action(socket, action)

    # multi.socket_action must call both SOCKETFUNCTION and TIMERFUNCTION at
    # various points during the transfer (at least at the start and end)
    def test_multi_socket_action(self):
        self.multi.add_handle(self.easy)
        self.handle_added = True
        self.timer_result = None
        self.socket_result = None
        self.multi.socket_action(pycurl.SOCKET_TIMEOUT, 0)
        assert self.socket_result is not None
        assert self.timer_result is not None

    # multi.add_handle must call TIMERFUNCTION to schedule a kick-start
    def test_multi_add_handle(self):
        self.multi.add_handle(self.easy)
        self.handle_added = True
        assert self.timer_result is not None

    # (mid-transfer) multi.remove_handle must call SOCKETFUNCTION to remove sockets
    def test_multi_remove_handle(self):
        self.multi.add_handle(self.easy)
        self.handle_added = True
        self.multi.socket_action(pycurl.SOCKET_TIMEOUT, 0)
        self.socket_result = None
        self.multi.remove_handle(self.easy)
        self.handle_added = False
        assert self.socket_result is not None

    # (mid-transfer) easy.pause(PAUSE_ALL) must call SOCKETFUNCTION to remove sockets
    # (mid-transfer) easy.pause(PAUSE_CONT) must call TIMERFUNCTION to resume
    def test_easy_pause_unpause(self):
        self.partial_transfer()
        self.socket_result = None
        # libcurl will now inform us that we should remove some sockets
        self.easy.pause(pycurl.PAUSE_ALL)
        assert self.socket_result is not None
        self.socket_result = None
        self.timer_result = None
        # libcurl will now tell us to add those sockets and schedule a kickstart
        self.easy.pause(pycurl.PAUSE_CONT)
        assert self.socket_result is not None
        assert self.timer_result is not None

    # (mid-transfer) easy.close() must call SOCKETFUNCTION to remove sockets
    def test_easy_close(self):
        self.partial_transfer()
        self.socket_result = None
        self.easy.close()
        assert self.socket_result is not None

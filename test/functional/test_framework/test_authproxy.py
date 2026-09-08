# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""RPC connection-loss regression: python3 -m unittest test_framework.test_authproxy."""
import http.client
import unittest
from unittest.mock import Mock

from .authproxy import AuthServiceProxy


class TestRPCReconnect(unittest.TestCase):
    def test_aborted_request_reconnects_once(self):
        # Inject the send failure observed on native Windows, while exercising
        # the real proxy's request, response parsing and retry bound.
        connection = Mock(spec=http.client.HTTPConnection)
        connection.request.side_effect = [ConnectionAbortedError(10053, "aborted"), None]
        response = connection.getresponse.return_value
        response.getheader.return_value = "application/json"
        response.read.return_value = b'{"result": 17, "error": null, "id": 1}'
        proxy = AuthServiceProxy("http://test:test@127.0.0.1:1", connection=connection)
        self.assertEqual(proxy.getblockcount(), 17)
        connection.close.assert_called_once_with()
        self.assertEqual(connection.request.call_count, 2)
        self.assertEqual(*connection.request.call_args_list)

    def test_repeated_abort_is_not_hidden(self):
        connection = Mock(spec=http.client.HTTPConnection)
        connection.request.side_effect = ConnectionAbortedError(10053, "aborted")
        proxy = AuthServiceProxy("http://test:test@127.0.0.1:1", connection=connection)
        with self.assertRaises(ConnectionAbortedError):
            proxy.getblockcount()
        self.assertEqual(connection.request.call_count, 2)
        connection.close.assert_called_once_with()

    def test_unrelated_error_is_not_retried(self):
        connection = Mock(spec=http.client.HTTPConnection)
        connection.request.side_effect = PermissionError("denied")
        proxy = AuthServiceProxy("http://test:test@127.0.0.1:1", connection=connection)
        with self.assertRaises(PermissionError):
            proxy.getblockcount()
        connection.request.assert_called_once()
        connection.close.assert_not_called()

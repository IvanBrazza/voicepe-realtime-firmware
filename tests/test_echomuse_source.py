import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
CPP = ROOT / "esphome/components/echomuse_client/echomuse_client.cpp"


class CredentialPersistenceTests(unittest.TestCase):
    def test_credentials_are_committed_as_one_nvs_update(self):
        source = CPP.read_text()
        store = source[source.index("bool EchoMuseClient::store_credentials_"):]
        commit = store.index("nvs_commit(handle)")
        for key in ('"ca"', '"token"', '"server"', '"port"', '"linked"'):
            self.assertLess(store.index(key), commit)

    def test_token_is_never_logged(self):
        source = CPP.read_text()
        log_lines = [line for line in source.splitlines() if "ESP_LOG" in line]
        self.assertFalse(any("token_" in line for line in log_lines))

    def test_linked_mode_never_builds_a_plaintext_uri(self):
        source = CPP.read_text()
        self.assertIn('std::string(linked_ ? "wss://" : "ws://")', source)
        self.assertNotIn("linked_ = false", source)

    def test_websocket_waits_for_network_before_connecting(self):
        source = CPP.read_text()
        self.assertIn('#include "esphome/components/network/util.h"', source)
        gate = source[source.index("if (reconnect_requested_"):source.index("connect_control_();")]
        self.assertIn("network::is_connected()", gate)


if __name__ == "__main__":
    unittest.main()

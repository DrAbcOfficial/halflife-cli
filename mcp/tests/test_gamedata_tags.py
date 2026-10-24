"""Version-tag compatibility tests for the gamedata synchronizer."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"

spec = importlib.util.spec_from_file_location("gamedata_tag_sync", SCRIPTS / "sync-gamedata.py")
sync = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = sync
spec.loader.exec_module(sync)


def index_document(game_version, content_addressed=False):
    sha256 = "0" * 64
    filename = (f"{game_version}.{sha256}.json" if content_addressed
                else f"{game_version}.json")
    return {"schemaVersion": 4, "versions": [{
        "gameVersion": game_version, "url": filename, "sha256": sha256,
        "size": 1, "snapshotSchemaVersion": 8, "fileCount": 1,
        "lastPublishTime": "2026-10-24T00:00:00Z",
    }]}


class GameVersionTagTests(unittest.TestCase):
    def parse_index(self, game_version, content_addressed=False):
        contents = json.dumps(index_document(game_version, content_addressed)).encode("utf-8")
        return sync.parse_index(contents, "fixture index")

    def load_manifest(self, game_version, field):
        document = {
            "schemaVersion": 1, "name": "test", "gameVersions": ["test-1"],
            "symbols": {"engine": {"Common": "function"}},
        }
        if field == "gameVersions":
            document[field] = [game_version]
        elif field == "symbolExemptions":
            document[field] = {"Common": [game_version]}
        else:
            document["conditionalGroups"] = [{
                field: [game_version], "symbols": {"engine": {"Conditional": "function"}},
            }]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            return sync.load_manifest(path)

    def test_index_accepts_decay_and_existing_versioned_tags(self):
        for tag in ("decay", "hl-8684", "svencoop-10257", "cstrikegameui-8684", "test_foo-1a"):
            for content_addressed in (False, True):
                with self.subTest(tag=tag, content_addressed=content_addressed):
                    parsed = self.parse_index(tag, content_addressed)
                    self.assertEqual([tag], [entry.game_version for entry in parsed.entries])

    def test_index_rejects_unknown_unversioned_and_malformed_tags(self):
        for tag in ("unknown", "Decay", "decay-extra", "hl-", "hl-abc", "../decay", "decay\n"):
            for content_addressed in (False, True):
                with self.subTest(tag=tag, content_addressed=content_addressed):
                    with self.assertRaisesRegex(sync.UpdateError, "invalid gameVersion"):
                        self.parse_index(tag, content_addressed)

    def test_manifest_accepts_decay_in_every_version_condition(self):
        for field in ("gameVersions", "symbolExemptions", "when", "exempt"):
            with self.subTest(field=field):
                manifest = self.load_manifest("decay", field)
                if field == "gameVersions":
                    actual = manifest.game_versions
                elif field == "symbolExemptions":
                    actual = manifest.symbol_exemptions["Common"]
                else:
                    actual = getattr(manifest.conditional_groups[0], field)
                self.assertEqual(("decay",), actual)

    def test_manifest_rejects_invalid_tags_in_every_version_condition(self):
        for field in ("gameVersions", "symbolExemptions", "when", "exempt"):
            for tag in ("unknown", "Decay", "decay-extra", "hl-", "hl-abc", "../decay", "decay\n"):
                with self.subTest(field=field, tag=tag):
                    with self.assertRaises(sync.UpdateError):
                        self.load_manifest(tag, field)

    def test_unselected_decay_does_not_block_manifest_selection(self):
        document = index_document("test-1")
        document["versions"].extend(index_document("decay", True)["versions"])
        parsed = sync.parse_index(json.dumps(document).encode("utf-8"), "mixed index")
        manifest = self.load_manifest("test-1", "gameVersions")
        selected = sync.select_manifest_entries(parsed, manifest)
        self.assertEqual(["test-1"], [entry.game_version for entry in selected])


if __name__ == "__main__":
    unittest.main()

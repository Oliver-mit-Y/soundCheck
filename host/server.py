from __future__ import annotations

import json
from pathlib import Path
from typing import Union

from flask import Flask, abort, jsonify, send_file


def create_app(data_dir: Union[str, Path, None] = None) -> Flask:
    base_dir = Path(data_dir) if data_dir is not None else Path(__file__).resolve().parent / "out"
    base_dir = base_dir.resolve()
    base_dir.mkdir(parents=True, exist_ok=True)

    app = Flask(__name__)

    @app.get("/health")
    def health() -> tuple[dict, int]:
        return jsonify({"status": "ok"}), 200

    @app.get("/api/info")
    def get_info():
        info_path = base_dir / "info.json"
        if not info_path.exists():
            return jsonify({"error": "info.json not found"}), 404

        with info_path.open("r", encoding="utf-8") as handle:
            return jsonify(json.load(handle))

    @app.get("/api/art")
    def list_art():
        art_dir = base_dir / "art"
        if not art_dir.exists():
            return jsonify({"files": []})

        files = sorted(path.name for path in art_dir.iterdir() if path.is_file())
        return jsonify({"files": files, "directory": str(art_dir)})

    @app.get("/api/art/<path:filename>")
    def get_art(filename: str):
        art_dir = base_dir / "art"
        if not art_dir.exists():
            return jsonify({"error": "art folder not found"}), 404

        candidate = (art_dir / filename).resolve()
        try:
            candidate.relative_to(art_dir.resolve())
        except ValueError:
            abort(403)

        if not candidate.exists() or not candidate.is_file():
            abort(404)

        return send_file(candidate)

    return app


app = create_app()


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=4567, debug=False)

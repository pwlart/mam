"""Strict, versioned job contracts. No automatic remote schema resolution."""
import json
import math
from pathlib import Path


def read_json(path):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f"Duplicate JSON key: {key}")
            result[key] = value
        return result

    def invalid(value):
        raise ValueError(f"Nonfinite JSON number: {value}")

    with Path(path).open(encoding="utf-8") as stream:
        result = json.load(stream, object_pairs_hook=pairs, parse_constant=invalid)
    def finite(value):
        if isinstance(value, float) and not math.isfinite(value):
            raise ValueError("Nonfinite JSON number")
        if isinstance(value, dict):
            for item in value.values(): finite(item)
        if isinstance(value, list):
            for item in value: finite(item)
    finite(result)
    return result


def validate_document(document, schema):
    from jsonschema import Draft202012Validator
    from referencing import Registry
    from referencing.exceptions import NoSuchResource

    if isinstance(schema, dict) and schema.get("$schema") not in (None, "https://json-schema.org/draft/2020-12/schema"):
        raise ValueError("This preflight adapter requires JSON Schema Draft 2020-12")

    def no_network(uri):
        raise NoSuchResource(ref=uri)

    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema, registry=Registry(retrieve=no_network))
    validator.validate(document)


def validate_job(job):
    schema = read_json(Path(__file__).with_name("job.schema.json"))
    validate_document(job, schema)
    operation = job["operation"]
    if operation != "chip" and "input" not in job:
        raise ValueError(f"{operation} requires input")
    if operation == "chip" and "input" in job:
        raise ValueError("chip does not accept input")
    if operation in {"decode", "export", "validate"}:
        if set(job["input"]) != {"path"}:
            raise ValueError(f"{operation} takes a whole file; use prepare for frame selection")
    schemas = schema["$defs"]["parameters"]
    validate_document(job.get("params", {}), schemas[operation])


def resolve_path(value, base):
    path = Path(value).expanduser()
    return path.resolve() if path.is_absolute() else (base / path).resolve()

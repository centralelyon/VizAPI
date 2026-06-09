# SportsData Module

This module provides validation of sports data against the [sportsdata](https://github.com/centralelyon/sportsdata) catalogs.

## Dependencies

The module requires the `sportsdata` Python package and its models repository:

```bash
# Install the sportsdata package
pip install git+https://github.com/centralelyon/sportsdata.git

# Clone the sportsdata repository for local models
# The module will automatically use it if found at data/external/sportsdata/
git clone https://github.com/centralelyon/sportsdata.git data/external/sportsdata
```

Alternatively, set the `SPORTSDATA_REPO_ROOT` environment variable to point to your sportsdata repository clone:

```bash
export SPORTSDATA_REPO_ROOT=/path/to/sportsdata
```

## Usage

### Fetch a catalog

```
GET /sportsdata/catalogs/common.sexes
```

Returns the catalog JSON from the sportsdata repository:

```json
{
  "id": "common.sexes",
  "title": "Sexes",
  "values": [
    {"id": "hommes", "label": "Men"},
    {"id": "femmes", "label": "Women"},
    {"id": "mixte", "label": "Mixed"}
  ]
}
```

### Validate a value

```
POST /sportsdata/validate
Content-Type: application/json

{
  "id": "common.sexes",
  "value": "hommes"
}
```

Returns:

```json
{
  "valid": true,
  "catalog_id": "common.sexes",
  "input_value": "hommes",
  "expected_values": ["hommes", "femmes", "mixte"],
  "message": null
}
```

For an invalid value:

```json
{
  "id": "common.sexes",
  "value": "tree"
}
```

Returns:

```json
{
  "valid": false,
  "catalog_id": "common.sexes",
  "input_value": "tree",
  "expected_values": ["hommes", "femmes", "mixte"],
  "message": "Value 'tree' is not in catalog 'common.sexes'"
}
```

## Priority

The module will try to load catalogs in this order:

1. **sportsdata_models Python package** - If installed and `SPORTSDATA_REPO_ROOT` is set to a cloned sportsdata repository
2. **GitHub HTTP fetch** - Fetches from https://github.com/centralelyon/sportsdata

## Test Data

Test JSON files are provided in `tests/modules/data/`:

- `common.sexes.valid.json` - Valid example with value "hommes"
- `common.sexes.nonvalid.json` - Invalid example with value "tree"

import pytest
from fastapi.testclient import TestClient

from app.main import app


@pytest.fixture
def client():
    return TestClient(app)


class TestSportsDataValidation:
    """Test sportsdata module validation endpoints"""

    def test_validate_valid_sex_value(self, client):
        """Test validating a valid value from common.sexes catalog"""
        request_data = {
            "id": "common.sexes",
            "value": "hommes"
        }
        response = client.post("/sportsdata/validate", json=request_data)
        assert response.status_code == 200
        data = response.json()
        assert data["valid"] is True
        assert data["catalog_id"] == "common.sexes"
        assert data["input_value"] == "hommes"
        assert "hommes" in data["expected_values"]
        assert "femmes" in data["expected_values"]
        assert "mixte" in data["expected_values"]
        assert data["message"] is None

    def test_validate_invalid_sex_value(self, client):
        """Test validating an invalid value (tree) from common.sexes catalog"""
        request_data = {
            "id": "common.sexes",
            "value": "tree"
        }
        response = client.post("/sportsdata/validate", json=request_data)
        assert response.status_code == 200
        data = response.json()
        assert data["valid"] is False
        assert data["catalog_id"] == "common.sexes"
        assert data["input_value"] == "tree"
        assert "hommes" in data["expected_values"]
        assert "femmes" in data["expected_values"]
        assert "mixte" in data["expected_values"]
        assert data["message"] is not None
        assert "tree" in data["message"]

    def test_validate_all_valid_sex_values(self, client):
        """Test all valid values from common.sexes catalog"""
        valid_values = ["hommes", "femmes", "mixte"]
        for value in valid_values:
            request_data = {
                "id": "common.sexes",
                "value": value
            }
            response = client.post("/sportsdata/validate", json=request_data)
            assert response.status_code == 200
            data = response.json()
            assert data["valid"] is True
            assert data["input_value"] == value

    def test_get_catalog_common_sexes(self, client):
        """Test fetching the common.sexes catalog"""
        response = client.get("/sportsdata/catalogs/common.sexes")
        assert response.status_code == 200
        data = response.json()
        assert data["id"] == "common.sexes"
        assert data["title"] == "Sexes"
        assert len(data["values"]) == 3
        
        # Check all expected values are present
        value_ids = [v["id"] for v in data["values"]]
        assert "hommes" in value_ids
        assert "femmes" in value_ids
        assert "mixte" in value_ids

    def test_get_catalog_not_found(self, client):
        """Test fetching a non-existent catalog"""
        response = client.get("/sportsdata/catalogs/nonexistent.catalog")
        assert response.status_code == 404

    def test_validate_nonexistent_catalog(self, client):
        """Test validating against a non-existent catalog"""
        request_data = {
            "id": "nonexistent.catalog",
            "value": "some_value"
        }
        response = client.post("/sportsdata/validate", json=request_data)
        assert response.status_code == 200
        data = response.json()
        assert data["valid"] is False
        assert data["expected_values"] == []
        assert "not found" in data["message"].lower()


class TestSportsDataNonValidJSON:
    """Test with the provided non-valid JSON file"""

    def test_nonvalid_json_from_file(self, client):
        """Test the non-valid JSON: {"common.sexes": "tree"}"""
        # This JSON has "common.sexes": "tree" which is not a valid value
        request_data = {
            "id": "common.sexes",
            "value": "tree"
        }
        response = client.post("/sportsdata/validate", json=request_data)
        assert response.status_code == 200
        data = response.json()
        assert data["valid"] is False
        assert data["input_value"] == "tree"
        assert "hommes" in data["expected_values"]
        assert "femmes" in data["expected_values"]
        assert "mixte" in data["expected_values"]


class TestSportsDataValidJSON:
    """Test with valid JSON examples"""

    def test_valid_json_hommes(self, client):
        """Test valid JSON with hommes value"""
        request_data = {
            "id": "common.sexes",
            "value": "hommes"
        }
        response = client.post("/sportsdata/validate", json=request_data)
        assert response.status_code == 200
        data = response.json()
        assert data["valid"] is True
        assert data["input_value"] == "hommes"

    def test_valid_json_femmes(self, client):
        """Test valid JSON with femmes value"""
        request_data = {
            "id": "common.sexes",
            "value": "femmes"
        }
        response = client.post("/sportsdata/validate", json=request_data)
        assert response.status_code == 200
        data = response.json()
        assert data["valid"] is True
        assert data["input_value"] == "femmes"

    def test_valid_json_mixte(self, client):
        """Test valid JSON with mixte value"""
        request_data = {
            "id": "common.sexes",
            "value": "mixte"
        }
        response = client.post("/sportsdata/validate", json=request_data)
        assert response.status_code == 200
        data = response.json()
        assert data["valid"] is True
        assert data["input_value"] == "mixte"

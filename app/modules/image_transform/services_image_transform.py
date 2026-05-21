from app.modules.image_transform.repositories_image_transform import (
    list_images,
    list_datasets,
)


def get_images():
    """List all available images."""
    return list_images()


def get_data():
    """List all available data files."""
    return list_datasets()

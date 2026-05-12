from app.types.module import Module

root = "descript-sketches"
module = Module(
    root=root,
    tag="Descript Sketches",
    # permissions=None,
)


@module.router.get(
    "/hello",
    response_model=str,
    status_code=200,
)
async def hello():
    return "Hello, I'm the Descript Sketches module!"

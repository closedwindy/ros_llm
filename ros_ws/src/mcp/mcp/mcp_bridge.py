from mcp.server.fastmcp import FastMCP

mcp = FastMCP("test",json_response=True)

#可用于调用可执行的驱动器
@mcp.tool()
def test()->int:
    """return 1"""
    return 1
#可用于获取ros提供的感知信息
@mcp.resource("greeting://{name}")
def get_greeting(name: str) -> str:
    """Get a personalized greeting"""
    return f"Hello, {name}!"

@mcp.prompt()
def greet_user(name: str, style: str = "friendly") -> str:
    """Generate a greeting prompt"""
    styles = {
        "friendly": "Please write a warm, friendly greeting",
        "formal": "Please write a formal, professional greeting",
        "casual": "Please write a casual, relaxed greeting",
    }

    return f"{styles.get(style, styles['friendly'])} for someone named {name}."



if __name__ == "__main__":
    mcp.run(transport="stdio")
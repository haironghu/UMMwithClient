"""
bmpclient/virtual_media_config.py — VirtualMedia 数据打散策略配置加载。

配置文件为 JSON 格式，默认路径为 bmpclient/config/virtual_media.json。
若文件不存在，则使用默认配置 {"strategy": "round_robin"}，保证向后兼容。
"""

import json
import os
from typing import Any, Dict, Optional


DEFAULT_STRATEGY = "round_robin"
DEFAULT_VIRTUAL_NODES = 150


def default_config() -> Dict[str, Any]:
    """返回默认配置。"""
    return {
        "strategy": DEFAULT_STRATEGY,
        "virtual_nodes": DEFAULT_VIRTUAL_NODES,
    }


def default_config_path() -> str:
    """返回默认配置文件路径：bmpclient/config/virtual_media.json。"""
    package_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(package_dir, "config", "virtual_media.json")


def load_config(path: Optional[str] = None) -> Dict[str, Any]:
    """
    加载 VirtualMedia 策略配置文件。

    :param path: 配置文件路径，None 则使用默认路径
    :return: 配置字典
    """
    path = path or default_config_path()

    cfg = default_config()
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            user_cfg = json.load(f)
        if not isinstance(user_cfg, dict):
            raise ValueError(f"config file {path} must contain a JSON object")
        cfg.update(user_cfg)

    return cfg


def get_strategy_name(config: Dict[str, Any]) -> str:
    """从配置中提取策略名。"""
    return str(config.get("strategy", DEFAULT_STRATEGY)).lower()


def get_strategy_options(config: Dict[str, Any]) -> Dict[str, Any]:
    """
    返回传递给策略构造函数的选项字典。

    排除顶级 strategy 字段，保留 virtual_nodes、hash_seed、module、class、options 等。
    """
    options = dict(config)
    options.pop("strategy", None)
    return options

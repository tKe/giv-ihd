import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.http_request import CONF_HTTP_REQUEST_ID, HttpRequestComponent
from esphome.const import CONF_ID, CONF_TIMEOUT

DEPENDENCIES = ["http_request"]

ha_history_ns = cg.esphome_ns.namespace("ha_history")
HAHistoryComponent = ha_history_ns.class_("HAHistoryComponent", cg.Component)

CONF_BASE_URL = "base_url"
CONF_TOKEN = "token"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HAHistoryComponent),
        cv.GenerateID(CONF_HTTP_REQUEST_ID): cv.use_id(HttpRequestComponent),
        cv.Required(CONF_BASE_URL): cv.url,
        cv.Required(CONF_TOKEN): cv.string,
        cv.Optional(CONF_TIMEOUT, default="8s"): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    http_request = await cg.get_variable(config[CONF_HTTP_REQUEST_ID])
    cg.add(var.set_http_request(http_request))
    cg.add(var.set_base_url(config[CONF_BASE_URL]))
    cg.add(var.set_token(config[CONF_TOKEN]))
    cg.add(var.set_timeout(config[CONF_TIMEOUT]))

idf_component_register(
	SRCS "RemoteSensors.cpp" "webserver.cpp"
	     "mongoose/mongoose.c"
	     "mongoose/mongoose_fs.c"
	     "mongoose/mongoose_glue.c"
	     "mongoose/mongoose_impl.c"
	INCLUDE_DIRS "." "mongoose"
	REQUIRES nvs_flash esp_netif esp_event esp_wifi esp_http_client mqtt driver app_update esp_timer json
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE MG_ENABLE_LOG=0)

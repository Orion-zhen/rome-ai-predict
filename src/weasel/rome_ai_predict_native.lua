local dir = rime_api.get_user_data_dir()
return assert(package.loadlib(dir .. "/rome-ai-predict/rome-ai-predict-weasel.dll", "luaopen_rome_ai_predict"))()

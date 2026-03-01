#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <thread>
#include <mutex>
#include <chrono>
#include <vector>
#include <string>
#include <memory>
#include <cstdio>

// llama.cpp 头文件
#include "llama.h"
#include "ggml-backend.h"

// Action 头文件
#include "llama_ros2/action/llm_infer.hpp"
//仿照llama.cpp示例实现在ros2环境下实现异步模型加载
class LlamaServer : public rclcpp::Node
{
public:
    LlamaServer() : Node("llama_server")
    {
        RCLCPP_INFO(this->get_logger(), "LLAMA ROS2 Action Server Starting...");
        
        using LLMInfer = llama_ros2::action::LLMInfer;
        action_server_ = rclcpp_action::create_server<LLMInfer>(
            this, "llama_infer",
            std::bind(&LlamaServer::handle_goal, this, std::placeholders::_1, std::placeholders::_2),
            std::bind(&LlamaServer::handle_cancel, this, std::placeholders::_1),
            std::bind(&LlamaServer::handle_accepted, this, std::placeholders::_1)
        );
        
        load_model_async();
        RCLCPP_INFO(this->get_logger(), "Action Server ready: /llama_infer");
    }

    ~LlamaServer()
    {
        if (sampler_) llama_sampler_free(sampler_);
        if (ctx_) llama_free(ctx_);
        if (model_) llama_model_free(model_);
        RCLCPP_INFO(this->get_logger(), "LlamaServer destroyed");
    }

private:
    // 模型指针
    llama_model * model_ = nullptr;
    llama_context * ctx_ = nullptr;
    const llama_vocab * vocab_ = nullptr;
    llama_sampler * sampler_ = nullptr;
    
    std::mutex model_mutex_;
    using LLMInfer = llama_ros2::action::LLMInfer;
    using GoalHandle = rclcpp_action::ServerGoalHandle<LLMInfer>;
    rclcpp_action::Server<LLMInfer>::SharedPtr action_server_;
    
    // Action 回调
    rclcpp_action::GoalResponse handle_goal(
        const rclcpp_action::GoalUUID &, std::shared_ptr<const LLMInfer::Goal> goal)
    {
        RCLCPP_INFO(this->get_logger(), "Goal: '%s' (max=%d)", 
                   goal->prompt.c_str(), goal->max_tokens);
        return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
    }
    
    rclcpp_action::CancelResponse handle_cancel(const std::shared_ptr<GoalHandle>)
    {
        RCLCPP_INFO(this->get_logger(), "Cancel requested");
        return rclcpp_action::CancelResponse::ACCEPT;
    }
    
    void handle_accepted(const std::shared_ptr<GoalHandle> goal_handle)
    {
        std::thread([this, goal_handle]() { execute_goal(goal_handle); }).detach();
    }
    
    //官方风格：LLM 推理
    void execute_goal(const std::shared_ptr<GoalHandle> goal_handle)
    {
        
        for (int i = 0; i < 100 && (!ctx_ || !vocab_); ++i) {
            if (goal_handle->is_canceling()) {
                auto res = std::make_shared<LLMInfer::Result>();
                res->success = false; res->response = "Cancelled";
                goal_handle->canceled(res); return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (!ctx_ || !vocab_) {
            auto res = std::make_shared<LLMInfer::Result>();
            res->success = false; res->response = "Model not ready";
            goal_handle->abort(res); return;
        }
        
        std::lock_guard<std::mutex> lock(model_mutex_);
        const auto goal = goal_handle->get_goal();
        auto t_start = std::chrono::high_resolution_clock::now();
        
        
        int n_prompt = -llama_tokenize(vocab_, goal->prompt.c_str(), goal->prompt.size(), nullptr, 0, true, true);
        if (n_prompt < 0) {
            auto res = std::make_shared<LLMInfer::Result>();
            res->success = false; res->response = "Tokenize failed";
            goal_handle->abort(res); return;
        }
        
        std::vector<llama_token> prompt_tokens(n_prompt);
        llama_tokenize(vocab_, goal->prompt.c_str(), goal->prompt.size(), 
                      prompt_tokens.data(), prompt_tokens.size(), true, true);
        
       
        llama_batch batch = llama_batch_get_one(prompt_tokens.data(), n_prompt);
        if (llama_model_has_encoder(model_)) {
            if (llama_encode(ctx_, batch)) {
                auto res = std::make_shared<LLMInfer::Result>();
                res->success = false; res->response = "Encode failed";
                goal_handle->abort(res); return;
            }
            llama_token dec_start = llama_model_decoder_start_token(model_);
            if (dec_start == LLAMA_TOKEN_NULL) dec_start = llama_vocab_bos(vocab_);
            batch = llama_batch_get_one(&dec_start, 1);
        } else {
            if (llama_decode(ctx_, batch)) {
                auto res = std::make_shared<LLMInfer::Result>();
                res->success = false; res->response = "Decode failed";
                goal_handle->abort(res); return;
            }
        }
        
       
        std::string output;
        const int n_predict = goal->max_tokens > 0 ? goal->max_tokens : 128;
        int n_decoded = 0;
        
        for (int n_pos = n_prompt; n_pos < n_prompt + n_predict; ) {
            if (goal_handle->is_canceling()) {
                auto res = std::make_shared<LLMInfer::Result>();
                res->success = false; res->response = output + "\n[Cancelled]";
                goal_handle->canceled(res); return;
            }
            
            // Sample
            llama_token new_token = llama_sampler_sample(sampler_, ctx_, -1);
            if (llama_vocab_is_eog(vocab_, new_token)) break;
            
            // Convert to text
            char buf[256];
            int n = llama_token_to_piece(vocab_, new_token, buf, sizeof(buf), 0, true);
            if (n > 0) output.append(buf, n);
            
            ++n_decoded;
            
            // Feedback
            if (n_decoded % 10 == 0) {
                auto fb = std::make_shared<LLMInfer::Feedback>();
                fb->tokens_generated = n_decoded;
                fb->progress = static_cast<float>(n_decoded) / n_predict;
                goal_handle->publish_feedback(fb);
            }
            
            // Next batch (official style: single token)
            batch = llama_batch_get_one(&new_token, 1);
            if (llama_decode(ctx_, batch)) break;
            ++n_pos;
        }
        
        // ===== Result =====
        auto t_end = std::chrono::high_resolution_clock::now();
        float latency = std::chrono::duration<float>(t_end - t_start).count();
        
        auto result = std::make_shared<LLMInfer::Result>();
        result->success = true;
        result->response = output;
        result->latency = latency;
        result->total_tokens = n_decoded;
        
        if (n_decoded > 0) {
            RCLCPP_INFO(this->get_logger(), "Done: %d tokens, %.3fs, %.1f t/s", 
                       n_decoded, latency, n_decoded/latency);
        }
        
        goal_handle->succeed(result);
        llama_sampler_reset(sampler_);
    }
    
    // 
    void load_model_async()
    {
        std::thread([this]() {
            const std::string model_path = "/home/hhw/Desktop/llama.cpp/Qwen3-0.6B-BF16.gguf";
            const int n_gpu_layers = 35;
            const int n_ctx = 512;
            
            RCLCPP_INFO(this->get_logger(), "Loading: %s", model_path.c_str());
            
            // Load model (official style)
            llama_model_params mparams = llama_model_default_params();
            mparams.n_gpu_layers = n_gpu_layers;
            model_ = llama_model_load_from_file(model_path.c_str(), mparams);
            if (!model_) {
                RCLCPP_ERROR(this->get_logger(), "Load failed"); return;
            }
            
            vocab_ = llama_model_get_vocab(model_);
            
            // Create context (official style)
            llama_context_params cparams = llama_context_default_params();
            cparams.n_ctx = n_ctx;
            cparams.n_batch = n_ctx;
            cparams.n_threads = 4;
            cparams.n_threads_batch = 4;
            ctx_ = llama_init_from_model(model_, cparams);
            if (!ctx_) {
                RCLCPP_ERROR(this->get_logger(), "Context failed");
                llama_model_free(model_); model_ = nullptr; return;
            }
            
            // Sampler chain (official style)
            auto sparams = llama_sampler_chain_default_params();
            sampler_ = llama_sampler_chain_init(sparams);
            llama_sampler_chain_add(sampler_, llama_sampler_init_temp(0.7f));
            llama_sampler_chain_add(sampler_, llama_sampler_init_dist(42));
            
            RCLCPP_INFO(this->get_logger(), "Loaded: %d layers, %d vocab", 
                       llama_model_n_layer(model_), llama_vocab_n_tokens(vocab_));
        }).detach();
    }
};

int main(int argc, char ** argv)
{
    // Init backends (once, in main)
    llama_backend_init();
    ggml_backend_load_all();
    
    // Init ROS 2
    rclcpp::init(argc, argv);
    
    // Run node
    auto node = std::make_shared<LlamaServer>();
    RCLCPP_INFO(rclcpp::get_logger("llama_server"), "Spinning...");
    rclcpp::spin(node);
    
    // Cleanup
    rclcpp::shutdown();
    llama_backend_free();
    
    return 0;
}
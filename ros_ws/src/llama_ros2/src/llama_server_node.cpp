#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <std_msgs/msg/string.hpp>
//#include <thread>
#include <mutex>
#include <chrono>
#include <vector>
#include <string>
#include <sstream>

// llama.cpp
#include "llama.h"
#include "ggml-backend.h"

// Action
#include "llama_ros2/action/robot_command.hpp"

// Function Calling
#include "../include/function_schema.h"

// 简单的JSON 解析
struct FunctionCall {
    std::string function;
    std::string location;
    std::string object;
    std::string message;
    float x = 0.0f, y = 0.0f, yaw = 0.0f;
    bool valid = false;
};
// 仿照llama.cpp源码加载模型和后端
class LlamaServer : public rclcpp::Node
{
public:
    LlamaServer() : Node("llama_server")
    {
        RCLCPP_INFO(this->get_logger(), "LLAMA ROS2 Function Calling Server Starting...");
        
        // 创建 Action Server
        action_server_ = rclcpp_action::create_server<llama_ros2::action::RobotCommand>(
            this,
            "robot_command",
            std::bind(&LlamaServer::handle_goal, this, std::placeholders::_1, std::placeholders::_2),
            std::bind(&LlamaServer::handle_cancel, this, std::placeholders::_1),
            std::bind(&LlamaServer::handle_accepted, this, std::placeholders::_1)
        );
        
        // 异步加载模型
        load_model_async();
        
        RCLCPP_INFO(this->get_logger(), "Action Server: /robot_command");
    }

    ~LlamaServer()
    {
        if (sampler_) llama_sampler_free(sampler_);
        if (ctx_) llama_free(ctx_);
        if (model_) llama_model_free(model_);
        llama_backend_free();
    }

private:
    // 模型相关
    llama_model * model_ = nullptr;
    llama_context * ctx_ = nullptr;
    const llama_vocab * vocab_ = nullptr;
    llama_sampler * sampler_ = nullptr;
    
    std::mutex model_mutex_;
    
    // Action Server
    rclcpp_action::Server<llama_ros2::action::RobotCommand>::SharedPtr action_server_;
    rclcpp_action::GoalHandle<llama_ros2::action::RobotCommand>::SharedPtr current_goal_;
    
    // Action Server 回调 
    rclcpp_action::GoalResponse handle_goal(
        const rclcpp_action::GoalUUID & uuid,
        std::shared_ptr<const llama_ros2::action::RobotCommand::Goal> goal)
    {
        RCLCPP_INFO(this->get_logger(), "Received goal: %s", goal->command_type.c_str());
        return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
    }
    
    rclcpp_action::CancelResponse handle_cancel(
        const std::shared_ptr<GoalHandle> goal_handle)
    {
        RCLCPP_INFO(this->get_logger(), "Received cancel request");
        return rclcpp_action::CancelResponse::ACCEPT;
    }
    
    void handle_accepted(const std::shared_ptr<GoalHandle> goal_handle)
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        current_goal_ = goal_handle;
        
        std::thread([this]() {
            execute_goal(current_goal_);
        }).detach();
    }
    
    // 执行 Goal（LLM 推理 + Function 调用）
    void execute_goal(const std::shared_ptr<GoalHandle> goal_handle)
    {
        if (!ctx_ || !vocab_) {
            auto result = std::make_shared<llama_ros2::action::RobotCommand::Result>();
            result->success = false;
            result->result_message = "Model not loaded";
            goal_handle->abort(result);
            return;
        }
        
        const auto goal = goal_handle->get_goal();
        
        // 构建 Prompt（包含 Function Calling 指令）
        std::string system_prompt = generate_system_prompt();
        std::string user_prompt = "User command: " + goal->command_type + " " + goal->target_name;
        
        std::string full_prompt = system_prompt + "\n\n" + user_prompt;
        
        //推理
        std::string llm_output = run_inference(full_prompt, 200);
        
        //解析 Function Call
        FunctionCall func_call = parse_function_call(llm_output);
        
        if (!func_call.valid) {
            auto result = std::make_shared<llama_ros2::action::RobotCommand::Result>();
            result->success = false;
            result->result_message = "Failed to parse function call: " + llm_output;
            goal_handle->abort(result);
            return;
        }
        
        //执行对应的 ROS Action
        bool success = execute_function(func_call, goal_handle);
        
        // 返回结果
        auto result = std::make_shared<llama_ros2::action::RobotCommand::Result>();
        result->success = success;
        result->result_message = success ? "Command executed" : "Command failed";
        result->execution_time = 0.0f;  // 执行时间
        
        if (success) {
            goal_handle->succeed(result);
        } else {
            goal_handle->abort(result);
        }
    }
    
 
    std::string run_inference(const std::string& prompt, int n_predict)
    {
        // Tokenize
        int n_prompt = -llama_tokenize(vocab_, prompt.c_str(), prompt.size(), 
                                       nullptr, 0, true, true);
        if (n_prompt < 0) return "";
        
        std::vector<llama_token> prompt_tokens(n_prompt);
        llama_tokenize(vocab_, prompt.c_str(), prompt.size(), 
                      prompt_tokens.data(), prompt_tokens.size(), 
                      true, true);
        
        // Batch
        llama_batch batch = llama_batch_get_one(prompt_tokens.data(), n_prompt);
        
        // Generate
        std::string generated_text;
        for (int i = 0; i < n_predict; i++) {
            if (llama_decode(ctx_, batch)) break;
            
            llama_token new_token_id = llama_sampler_sample(sampler_, ctx_, -1);
            if (llama_vocab_is_eog(vocab_, new_token_id)) break;
            
            char buf[256];
            int n = llama_token_to_piece(vocab_, new_token_id, buf, sizeof(buf), 0, true);
            if (n > 0) generated_text.append(buf, n);
            
            batch = llama_batch_get_one(&new_token_id, 1);
        }
        
        llama_sampler_reset(sampler_);
        return generated_text;
    }
    

    FunctionCall parse_function_call(const std::string& json)
    {
        FunctionCall call;
        
        // 简单字符串查找（生产环境用 proper JSON parser）
        if (json.find("\"navigate\"") != std::string::npos) {
            call.function = "navigate";
            call.valid = true;
            // 解析参数...
        } else if (json.find("\"grab\"") != std::string::npos) {
            call.function = "grab";
            call.valid = true;
        } else if (json.find("\"stop\"") != std::string::npos) {
            call.function = "stop";
            call.valid = true;
        } else if (json.find("\"say\"") != std::string::npos) {
            call.function = "say";
            call.valid = true;
        }
        
        return call;
    }
    
    //执行 Function 
    bool execute_function(const FunctionCall& call, 
                         const std::shared_ptr<GoalHandle> goal_handle)
    {
        RCLCPP_INFO(this->get_logger(), "Executing function: %s", call.function.c_str());
        
        if (call.function == "navigate") {
            // 发送导航 Action
            return execute_navigate(call, goal_handle);
        } else if (call.function == "grab") {
            return execute_grab(call, goal_handle);
        } else if (call.function == "say") {
            return execute_say(call, goal_handle);
        } else if (call.function == "stop") {
            return execute_stop(call, goal_handle);
        }
        
        return false;
    }
    
    bool execute_navigate(const FunctionCall& call,
                         const std::shared_ptr<GoalHandle> goal_handle)
    {
        // 这里可以调用 nav2 的 NavigateToPose Action
        // 测试版：直接返回成功
        
        // 发送反馈
        auto feedback = std::make_shared<llama_ros2::action::RobotCommand::Feedback>();
        feedback->progress = 0.5f;
        feedback->status_message = "Navigating to " + call.location;
        goal_handle->publish_feedback(feedback);
        
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        feedback->progress = 1.0f;
        feedback->status_message = "Navigation complete";
        goal_handle->publish_feedback(feedback);
        
        return true;
    }
    
    bool execute_grab(const FunctionCall& call,
                     const std::shared_ptr<GoalHandle> goal_handle)
    {
        auto feedback = std::make_shared<llama_ros2::action::RobotCommand::Feedback>();
        feedback->progress = 1.0f;
        feedback->status_message = "Grabbing " + call.object;
        goal_handle->publish_feedback(feedback);
        
        return true;
    }
    
    bool execute_say(const FunctionCall& call,
                    const std::shared_ptr<GoalHandle> goal_handle)
    {
        RCLCPP_INFO(this->get_logger(), "Robot says: %s", call.message.c_str());
        
        auto feedback = std::make_shared<llama_ros2::action::RobotCommand::Feedback>();
        feedback->progress = 1.0f;
        feedback->status_message = call.message;
        goal_handle->publish_feedback(feedback);
        
        return true;
    }
    
    bool execute_stop(const FunctionCall& call,
                     const std::shared_ptr<GoalHandle> goal_handle)
    {
        RCLCPP_INFO(this->get_logger(), "Stopping current action");
        return true;
    }
    
    //模型加载
    void load_model_async()
    {
        std::thread([this]() {
            llama_backend_init();
            ggml_backend_load_all();
            
            std::string model_path = "/home/hhw/Desktop/llama.cpp/Qwen3-0.6B-BF16.gguf";
            int n_gpu_layers = 35;
            int n_ctx = 512;
            
            llama_model_params mparams = llama_model_default_params();
            mparams.n_gpu_layers = n_gpu_layers;
            
            model_ = llama_model_load_from_file(model_path.c_str(), mparams);
            if (!model_) {
                RCLCPP_ERROR(this->get_logger(), "Failed to load model!");
                return;
            }
            
            vocab_ = llama_model_get_vocab(model_);
            
            llama_context_params cparams = llama_context_default_params();
            cparams.n_ctx = n_ctx;
            cparams.n_batch = n_ctx;
            
            ctx_ = llama_init_from_model(model_, cparams);
            if (!ctx_) {
                RCLCPP_ERROR(this->get_logger(), "Failed to create context!");
                return;
            }
            
            auto sparams = llama_sampler_chain_default_params();
            sampler_ = llama_sampler_chain_init(sparams);
            llama_sampler_chain_add(sampler_, llama_sampler_init_temp(0.7f));
            llama_sampler_chain_add(sampler_, llama_sampler_init_dist(42));
            
            RCLCPP_INFO(this->get_logger(), "Model loaded successfully");
        }).detach();
    }
};

int main(int argc, char ** argv)
{
    rclcpp::init(argc, argv);
    
    llama_backend_init();
    ggml_backend_load_all();
    
    auto node = std::make_shared<LlamaServer>();
    
    rclcpp::spin(node);
    
    rclcpp::shutdown();
    llama_backend_free();
    
    return 0;
}
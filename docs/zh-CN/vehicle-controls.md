[English](../vehicle-controls.md) | [简体中文](vehicle-controls.md)

# 可选 Vehicle State Control

Vehicle State 处理器现在支持注册带类型化请求/响应参数的服务级 Control。线格式依据 LIVI 版本 64a05b68358db52ed887169143afd5944fd6f1f7 的 vehicle_state.rs，重点为 Control::describe、accessory_list 和 control。作者 Lasse Heitgres（https://github.com/f-io/LIVI），署名见 NOTICE。此前所有 controlRequest 均返回未知命令。默认不提供命令、执行器、生产类型/IID、图像包或新增能力，D213 模拟发动机配置不注册 Control。这是可移植协议支持，不能证明官方 Ultra 或物理动作。

## 目录与归属

创建模型前在 rtcp_vehicle_config_t 填写 controls/control_count 和 prepare_control，可选 did_control 负责编码后的提交。每个 Control 必须引用已有且包含特征的服务，IID 为正且不得与任何配件、服务、特征或其他 Control 冲突，type 为宿主提供的非负值。当前最多四个 Control，每个最多四个必需请求参数和四个响应参数。参数名在各自模式内唯一，包含 1..32 字节可打印 ASCII。值使用已有 Vehicle 基础类型；STRING/DATA 需明确 1..256 字节上限，此接口不实现嵌套字典/数组及更大的图像包。

创建时校验并复制描述、名称和模式数组，回调上下文借用至销毁。所有调用及回调均在串行所有者线程执行，回调不得重入。只有注册 Control 才分配有界工作区；未注册时保持原有元数据及未知命令响应行为。

configResponse 和 vehicleStateProtocolInfo 在对应服务下包含 controls，每项有 iid/type、sender=0 和 hasResponse=true；urgent 增加 priority=1。非空 requestParameters/responseParameters 数组包含 name/format，遵循参考实现。本地 maximum_length 仅用于值校验，不臆造线格式字段。原有 /info 一致性检查包含完整 Control 目录，Receiver Vehicle State 协商门控会拒绝刷新后不匹配的模式。注册本身不会启用 vehicleStateProtocol、uiSync、altScreen 或渲染器。

## 请求、准备与提交

controlRequest 的 values 使用整数 IID 键，每个已注册命令的参数为具名参数字典；无参数命令接受 null 或空字典。必需参数数量、名称、类型及本地边界必须完全匹配，重复命令 IID 或匹配命令的畸形参数在任何宿主准备前拒绝。未知 Control 沿用参考值 -6735 作为逐 IID 错误，已知/未知混合批次返回成功结果及未知错误，transactionID 保持原有有符号整数/null 回显契约。

prepare_control 按描述顺序接收复制参数，按描述顺序填写复制的响应值，必须是纯准备，不能执行物理动作或其他副作用，因为输出不足或输入重试时可能再次执行。返回 false 产生本地 ESTATE，响应模式/值无效产生 EINVAL，不臆造手机侧逐命令错误码。任一已注册命令拒绝或无效时，整批均不会执行提交回调；后续准备拒绝前的纯准备可能已经执行。

仅当完整响应成功编码且适配输出容量后，did_control 才按请求顺序执行。无响应参数的命令返回 null，否则返回描述名称与类型值组成的字典。状态增加已注册 Control 数及 committed_controls，累计至模型销毁。提交仅表示编码及本地交接，不代表响应送达或车辆反馈。只读命令可以省略 did_control。

本实现不推测恰好执行一次的策略，重复有效请求可能再次提交；非幂等动作必须使用宿主已确认的事务/执行器契约，不新增猜测的重放窗口、超时或物理反馈规则。会话重置保留 Control 注册和累计统计，沿用现有值/订阅重置行为。

Receiver 在传输队列满时保留已编码响应，重试这些字节不会再次准备或提交。前一响应仍无法入队时的新请求沿用既有策略，在进入处理器前拒绝。畸形或拒绝的 Control 按现有应用拒绝策略只退役请求所在通道，重开需使用新传输 seed。本地提交后响应丢失不能回滚实际宿主动作。

## Host 证据及剩余验收

专用单元测试覆盖描述/名称复制、重复/冲突/未知服务 IID、缺少准备回调、参数数量/类型/边界、畸形批次在准备前拒绝、纯准备拒绝、无效准备输出、所有请求截断和不足输出容量、已知/未知混合响应、二进制响应、双通道及重置保留。独立 Python 编码输入 OPACK、解码输出，并用 plistlib 验证实际生成的服务目录及重排/不匹配元数据。

实际 Receiver 交付工作区测试保持传输队列满，验证保留响应重试不重复执行、后续请求在进入处理器前拒绝、恢复后仅发送一次，以及丢弃响应不会撤销本地提交。

--vehicle-controls 公共 Receiver 场景使用真实 TCP、Pair-Verify、ControlCipher 及两条 Vehicle 通道，在两条通道执行具类型的混合批次，拒绝含后续畸形命令的请求且不提交前一个命令，验证 HIGH 读取仍可继续，以新 seed 重开后接受命令，随后继续通知、Classic 音视频及控制器重连回归。合成 IID 和载荷不能证明真实 iPhone Vehicle Control 互操作、官方目录语义或物理动作。完整范围 L1 仍为 PARTIAL，L2–L5/官方验收仍为 NOT_RUN。

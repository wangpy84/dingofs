��          �      �       0  $   1     V  7   r     �  �   �     >  #   W     {  �   �  .   /     ^     r  O  �  #   �     �  3        B  �   O     �          3  l   L  -   �     �     �                     
   	                                  1. Add new machines to the host list 2. Submit the list of hosts Add the list of expanded services to the topology file: Expansion Cluster For the metaserver service, a new logical pool is created every time you scale up, and the new services are located in this pool. Step 1: Commit host list Step 2: Modify the cluster topology Step 3: Expand the cluster The scale-out operation is an idempotent operation, so the user can repeat the operation if it fails, so don't worry about the service residual problem. You can only scale services with the same role ⚠️ **Warning:** 💡 **REMINDER:** Project-Id-Version: DingoFS
Report-Msgid-Bugs-To: 
PO-Revision-Date: 2025-06-06 12:03+0800
Last-Translator: 
Language-Team: zh_CN <LL@li.org>
Language: zh_CN
MIME-Version: 1.0
Content-Type: text/plain; charset=utf-8
Content-Transfer-Encoding: 8bit
Plural-Forms: nplurals=1; plural=0;
Generated-By: Babel 2.17.0
X-Generator: Poedit 3.6
 1.添加新增机器至主机列表 2.提交主机列表 将扩容的服务列表添加到拓扑文件中： 扩容集群 对于 metaserver 服务来说，每次扩容都会新增一个逻辑池，新增的服务都位于该逻辑池中，请确保每次扩容至少增加 3 台主机。 第 1 步：提交主机列表 第 2 步：修改集群拓扑 第 3 步：扩容集群 扩容操作属于幂等操作，用户在执行失败后可重复执行，不用担心服务残留问题。 每一次只能扩容同一种角色的服务 ⚠️ **警告** 💡 **提醒** 
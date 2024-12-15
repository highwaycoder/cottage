#!/bin/bash

# info: lazygit: https://github.com/jesseduffield/lazygit
#       fjira: https://github.com/mk-5/fjira/

tmux new -s dev-cottage -d

tmux rename-window -t dev-cottage -n vim
tmux send-keys -t dev-cottage:0 "vim" Enter

tmux new-window -t dev-cottage -n lazygit
tmux send-keys -t dev-cottage:1 "lazygit" Enter

tmux new-window -t dev-cottage -n terminal
tmux attach -t dev-cottage:2



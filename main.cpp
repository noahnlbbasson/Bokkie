#include "Agent.h"
#include "md_render.h"
#include "workspaces/Workspace.h"
#include "LandingScreen.h"
#include "WorkspacesScreen.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    enable_ansi();
    try {
        Workspace workspaces;

        // Bokkie <project folder> [chat name] opens (or creates) that workspace and chat directly.
        if (argc >= 2) {
            const auto made = workspaces.createWorkspaceForProject(Workspace::parseUserPath(argv[1]));
            if (!made.ok) {
                std::cerr << made.error << '\n';
                return 1;
            }
            const std::string chat = argc >= 3 ? argv[2] : "main";
            if (!workspaces.createChat(made.name, chat) &&
                !workspaces.loadChat(made.name, chat).is_object()) {
                std::cerr << "Could not create the chat \"" << chat << "\"\n";
                return 1;
            }
            Agent agent(workspaces, ChatTarget{false, made.name, chat});
            agent.run();
            return 0;
        }

        LandingScreen landing(workspaces);
        while (true) {
            const LandingChoice choice = landing.displayLandingScreen();
            ChatTarget target;
            std::string banner = "Quick chat.";

            switch (choice.action) {
                case LandingChoice::Action::Quit:
                    return 0;
                case LandingChoice::Action::QuickChat:
                    break;
                case LandingChoice::Action::OpenChat:
                    target = ChatTarget{false, choice.workspace, choice.chat};
                    banner = choice.workspace + " / " + choice.chat + ".";
                    break;
                case LandingChoice::Action::Workspaces: {
                    WorkspacesScreen screen(workspaces);
                    const WorkspacesResult result = screen.run();
                    if (result.action == WorkspacesResult::Action::OpenChat) {
                        target = ChatTarget{false, result.workspace, result.chat};
                        banner = result.workspace + " / " + result.chat + ".";
                        break;
                    }
                    if (!result.notice.empty()) landing.setNotice(result.notice);
                    continue;
                }
                case LandingChoice::Action::Settings:
                    landing.setNotice("The Settings screen isn't built yet.");
                    continue;
            }

            std::cout << "\033[2J\033[H" << md_ansi::dim << banner
                      << " Type :b to go back to the menu, :q to quit."
                      << md_ansi::reset << "\n\n";
            try {
                Agent agent(workspaces, target);
                if (!agent.run()) break;
            } catch (const std::exception& error) {
                landing.setNotice(error.what());
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
